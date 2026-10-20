// Included by overlay.cpp inside rt::ui: reuses its actual model and dialog layout.
// CoreGraphics/CoreText rasterise a transparent premultiplied RGBA canvas; Metal
// composites it over the backbuffer. No AppKit, NSApplication, window or audio API.
namespace {

struct Snapshot {
  std::vector<Row> rows;
  int page = 0, focus = 0, slot = 0, scroll = 0;
  bool capturing = false, closing = false;
  std::string toast;
};

template<class T> void mac_release(T*& value) { if (value) value->Release(); value = nullptr; }

class MacCanvas {
 public:
  int width, height;
  std::vector<u8> pixels;
  CGContextRef cg = nullptr;
  explicit MacCanvas(int w = int(kW), int h = int(kH))
      : width(w), height(h), pixels(size_t(w) * size_t(h) * 4) {
    const auto space = CGColorSpaceCreateDeviceRGB();
    cg = CGBitmapContextCreate(pixels.data(), size_t(width), size_t(height), 8, size_t(width) * 4, space,
                               CGBitmapInfo(kCGImageAlphaPremultipliedLast) | CGBitmapInfo(kCGBitmapByteOrder32Big));
    CGColorSpaceRelease(space);
    if (cg) {
      CGContextTranslateCTM(cg, 0, height);
      CGContextScaleCTM(cg, 1, -1);  // model coordinates have their origin at the top left
      CGContextSetShouldAntialias(cg, true);
    }
  }
  ~MacCanvas() { if (cg) CGContextRelease(cg); }
  MacCanvas(const MacCanvas&) = delete;
  MacCanvas& operator=(const MacCanvas&) = delete;

  void fill(const Box& b, float red, float green, float blue, float alpha = 1) {
    CGContextSetRGBFillColor(cg, red, green, blue, alpha);
    CGContextFillRect(cg, CGRectMake(b.x0, b.y0, b.x1 - b.x0, b.y1 - b.y0));
  }
  void line(float x0, float y0, float x1, float y1, float gray) {
    CGContextSetRGBStrokeColor(cg, gray, gray, gray, 1);
    CGContextSetLineWidth(cg, 1);
    CGContextMoveToPoint(cg, x0, y0); CGContextAddLineToPoint(cg, x1, y1); CGContextStrokePath(cg);
  }
  void frame(const Box& b, float gray = 0.55f, float inner = 0.28f) {
    CGContextSetRGBStrokeColor(cg, gray, gray, gray, 1);
    CGContextStrokeRectWithWidth(cg, CGRectMake(b.x0 + 0.5f, b.y0 + 0.5f, b.x1 - b.x0 - 1, b.y1 - b.y0 - 1), 1);
    CGContextSetRGBStrokeColor(cg, inner, inner, inner, 1);
    CGContextStrokeRectWithWidth(cg, CGRectMake(b.x0 + 3.5f, b.y0 + 3.5f, b.x1 - b.x0 - 7, b.y1 - b.y0 - 7), 1);
  }
  void selected(const Box& b) { fill(b, 0.20f, 0.25f, 0.30f); }
  void triangle(float x, float y, float dx, float dy, float gray) {
    const float px = -dy, py = dx;
    CGContextSetRGBFillColor(cg, gray, gray, gray, 1);
    CGContextMoveToPoint(cg, x + 8 * dx, y + 8 * dy);
    CGContextAddLineToPoint(cg, x - 5 * dx + 7 * px, y - 5 * dy + 7 * py);
    CGContextAddLineToPoint(cg, x - 5 * dx - 7 * px, y - 5 * dy - 7 * py);
    CGContextClosePath(cg); CGContextFillPath(cg);
  }

  void text(CFStringRef string, const Box& b, float size, float gray = 0.86f,
            CTTextAlignment align = kCTTextAlignmentLeft, bool serif = false, bool wrap = false) {
    if (!string || CFStringGetLength(string) == 0 || b.x1 <= b.x0 || b.y1 <= b.y0) return;
    const auto font = CTFontCreateWithName(serif ? CFSTR("TimesNewRomanPSMT") : CFSTR("Helvetica"), size, nullptr);
    const CGFloat rgba[] = {gray, gray, gray, 1};
    const auto space = CGColorSpaceCreateDeviceRGB();
    const auto color = CGColorCreate(space, rgba);
    CGColorSpaceRelease(space);
    const CTLineBreakMode break_mode = wrap ? kCTLineBreakByWordWrapping : kCTLineBreakByClipping;
    const CTParagraphStyleSetting settings[] = {{kCTParagraphStyleSpecifierAlignment, sizeof align, &align},
                                                {kCTParagraphStyleSpecifierLineBreakMode, sizeof break_mode, &break_mode}};
    const auto paragraph = CTParagraphStyleCreate(settings, 2);
    const void* keys[] = {kCTFontAttributeName, kCTForegroundColorAttributeName, kCTParagraphStyleAttributeName};
    const void* values[] = {font, color, paragraph};
    const auto attributes = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 3,
                                                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const auto attributed = CFAttributedStringCreate(kCFAllocatorDefault, string, attributes);
    const auto setter = CTFramesetterCreateWithAttributedString(attributed);
    const CGFloat width = b.x1 - b.x0, height = b.y1 - b.y0;
    const auto measured = CTFramesetterSuggestFrameSizeWithConstraints(setter, CFRangeMake(0, 0), nullptr,
                                                                      CGSizeMake(width, CGFLOAT_MAX), nullptr);
    const CGFloat text_height = std::min(height, std::ceil(measured.height) + 2);
    const auto path = CGPathCreateWithRect(CGRectMake(0, (height - text_height) / 2, width, text_height), nullptr);
    const auto frame = CTFramesetterCreateFrame(setter, CFRangeMake(0, 0), path, nullptr);
    CGContextSaveGState(cg);
    CGContextClipToRect(cg, CGRectMake(b.x0, b.y0, width, height));
    CGContextTranslateCTM(cg, b.x0, b.y1);
    CGContextScaleCTM(cg, 1, -1);  // CoreText lines use a bottom-left text coordinate system
    CGContextSetTextMatrix(cg, CGAffineTransformIdentity);
    CTFrameDraw(frame, cg);
    CGContextRestoreGState(cg);
    CFRelease(frame); CFRelease(path); CFRelease(setter); CFRelease(attributed); CFRelease(attributes);
    CFRelease(paragraph); CGColorRelease(color); CFRelease(font);
  }
  void text(const std::string& string, const Box& b, float size, float gray = 0.86f,
            CTTextAlignment align = kCTTextAlignmentLeft, bool serif = false, bool wrap = false) {
    const auto value = CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(string.data()),
                                               CFIndex(string.size()), kCFStringEncodingUTF8, false);
    text(value, b, size, gray, align, serif, wrap);
    if (value) CFRelease(value);
  }
  void text(const std::u16string& string, const Box& b, float size, float gray = 0.86f) {
    static_assert(sizeof(char16_t) == sizeof(UniChar));
    const auto value = CFStringCreateWithCharacters(kCFAllocatorDefault, reinterpret_cast<const UniChar*>(string.data()),
                                                     CFIndex(string.size()));
    text(value, b, size, gray);
    if (value) CFRelease(value);
  }
};

void mac_draw_page(MacCanvas& canvas, const Snapshot& s) {
  canvas.fill({0, 0, kW, kH}, 0, 0, 0);
  if (s.closing) return;
  canvas.text("PC SETTINGS", {0, 34, 326, 76}, 40, 0.86f, kCTTextAlignmentRight, true);
  canvas.text("DISPLAY", {0, 74, 326, 101}, 25, 0.86f, kCTTextAlignmentRight, true);
  canvas.line(0, 74.5f, 348, 74.5f, 0.62f);
  canvas.line(348.5f, 0, 348.5f, 101, 0.62f);
  canvas.line(0, 101.5f, 348, 101.5f, 0.62f);
  canvas.line(364, 101.5f, kRowR + 1, 101.5f, 0.62f);
  const int count = int(s.rows.size()) - 1;
  for (int visible = 0; visible < kVisibleRows && s.scroll + visible < count; ++visible) {
    const int index = s.scroll + visible;
    const auto& row = s.rows[size_t(index)];
    const float y0 = kListTop + float(visible) * kPitch, y1 = y0 + kRowH, ym = (y0 + y1) / 2;
    if (index == s.focus) canvas.selected({kRowL + 1, y0 + 1, kRowR - 1, y1 - 1});
    canvas.frame({kRowL, y0, kRowR, y1});
    const float gray = row.enabled ? 0.83f : 0.42f;
    canvas.text(row.label, {94, y0, 326, y1}, 25, gray, kCTTextAlignmentLeft, true);
    canvas.line(332.5f, y0 + 7, 332.5f, y1 - 7, 0.4f);
    canvas.triangle(kArrowL, ym, -1, 0, row.enabled ? 0.6f : 0.25f);
    canvas.triangle(kArrowR, ym, 1, 0, row.enabled ? 0.6f : 0.25f);
    if (row.kind == Kind::Choice && row.index >= 0 && row.index < int(row.options.size()))
      canvas.text(row.options[size_t(row.index)], {kBarL, y0, kBarR, y1}, 20, gray, kCTTextAlignmentCenter);
    else if (row.kind == Kind::Slider) {
      canvas.text(row.value_note, {340, y0, 463, y1}, 14, 0.8f, kCTTextAlignmentCenter);
      const float fraction = row.hi > row.lo ? float(std::clamp((row.value - row.lo) / (row.hi - row.lo), 0.0, 1.0)) : 0;
      canvas.fill({kBarL, ym - 5, kBarL + (kBarR - kBarL) * fraction, ym + 6}, 0.3f, 0.3f, 0.3f);
      canvas.frame({kBarL, ym - 6, kBarR, ym + 6}, 0.5f, 0.3f);
    }
  }
  for (int button = 0; button < 2; ++button) {
    const Box box{kButtonX[button], kButtonsY, kButtonX[button] + kButtonW, kButtonsY + kRowH};
    if (s.focus == count && s.slot == button) canvas.selected(box);
    canvas.frame(box);
    canvas.text(button ? "CLOSE" : "DEFAULT", box, 22, 0.86f, kCTTextAlignmentCenter, true);
  }
  canvas.line(0, 638.5f, kW, 638.5f, 0.62f);
  canvas.line(0, 676.5f, kW, 676.5f, 0.62f);
  canvas.text(s.toast.empty() ? "D-PAD / LEFT STICK  MOVE     A / CROSS  SELECT     B / CIRCLE  BACK" : s.toast,
              {96, 640, kW - 40, 675}, 18, 0.9f);
}

void mac_draw_dialog(MacCanvas& canvas, const DialogView& v) {
  canvas.fill({0, 0, kW, kH}, 0, 0, 0, 0.6f);
  const auto layout = dialog_layout(v);
  canvas.fill(layout.panel, 0.035f, 0.04f, 0.05f, 0.97f);
  canvas.frame(layout.panel);
  canvas.text(v.heading, layout.heading, 30, 0.86f, kCTTextAlignmentLeft, true);
  canvas.line(layout.heading.x0, layout.rule_y + 0.5f, layout.heading.x1, layout.rule_y + 0.5f, 0.62f);
  if (v.kind == DialogView::List) {
    for (size_t i = 0; i < layout.rows.size(); ++i) {
      const int row = v.scroll + int(i);
      const auto& box = layout.rows[i];
      if (row == v.focus) canvas.selected(box);
      canvas.frame(box);
      canvas.text(v.rows[size_t(row)], {box.x0 + 16, box.y0, box.x1 - 200, box.y1}, 23);
      if (size_t(row) < v.dates.size()) canvas.text(v.dates[size_t(row)], {box.x1 - 200, box.y0, box.x1 - 16, box.y1}, 15, 0.62f);
    }
    if (v.focus >= 0 && size_t(v.focus) < v.details.size())
      canvas.text(v.details[size_t(v.focus)], layout.detail, 19, 0.72f, kCTTextAlignmentLeft, false, true);
    if (!layout.rows.empty() && v.scroll > 0) canvas.triangle(layout.rows.front().x1 + 13, layout.rows.front().y0 + 12, 0, -1, 0.5f);
    if (!layout.rows.empty() && v.scroll + kDialogListRows < int(v.rows.size()))
      canvas.triangle(layout.rows.back().x1 + 13, layout.rows.back().y1 - 12, 0, 1, 0.5f);
  } else if (v.kind == DialogView::Text) {
    canvas.text(v.text, layout.text, 20, 0.8f, kCTTextAlignmentLeft, false, true);
    if (v.edit_selected) canvas.selected(layout.edit);
    canvas.frame(layout.edit);
    const auto& edit = layout.edit;
    canvas.text(v.edit_selected ? v.edit : v.edit + u"_", {edit.x0 + 16, edit.y0, edit.x1 - 110, edit.y1}, 23, 0.9f);
    canvas.text(std::to_string(v.edit.size()) + " / " + std::to_string(v.max_chars),
                {edit.x1 - 110, edit.y0, edit.x1 - 16, edit.y1}, 15, 0.55f, kCTTextAlignmentRight);
    for (size_t i = 0; i < layout.keys.size(); ++i) {
      const auto& box = layout.keys[i];
      if (int(i) == v.key) canvas.selected(box);
      canvas.frame(box, int(i) == v.key ? 0.75f : 0.35f);
      const auto label = v.keys[i] == u' ' ? std::u16string(u"SPACE") : std::u16string(1, v.keys[i]);
      const auto string = CFStringCreateWithCharacters(kCFAllocatorDefault, reinterpret_cast<const UniChar*>(label.data()), CFIndex(label.size()));
      canvas.text(string, box, 19, 0.9f, kCTTextAlignmentCenter);
      CFRelease(string);
    }
    canvas.text("D-PAD / LEFT STICK + A / CROSS  TYPE     X / SQUARE  DELETE     Y / TRIANGLE  SPACE     START  OK     B / CIRCLE  BACK",
                layout.help, 16, 0.65f, kCTTextAlignmentLeft, false, true);
  } else {
    canvas.text(v.text, layout.text, 20, 0.86f, kCTTextAlignmentLeft, false, true);
  }
  for (int i = 0; i < layout.buttons_n; ++i) {
    const auto& box = layout.buttons[i];
    if (v.button == i) canvas.selected(box);
    canvas.frame(box);
    const char* label = v.kind == DialogView::Question ? (i ? "NO" : "YES") : i ? "BACK" : "OK";
    canvas.text(label, box, 22, 0.86f, kCTTextAlignmentCenter, true);
  }
}

// Include every field that affects pixels; cached pixels are uploaded only after model changes.
std::string mac_raster_key(bool page, const Snapshot& s, bool dialog, const DialogView& d) {
  std::string key;
  auto number = [&](auto value) { key += std::to_string(value) + "/"; };
  auto string = [&](const std::string& value) { number(value.size()); key += value; };
  number(page); number(dialog);
  if (page) {
    number(s.rows.size()); number(s.closing); number(s.page); number(s.focus); number(s.slot); number(s.scroll); string(s.toast);
    for (const auto& row : s.rows) {
      number(int(row.kind)); string(row.label); string(row.value_note); number(row.index); number(row.enabled);
      number(row.value); number(row.lo); number(row.hi);
      number(row.options.size()); for (const auto& option : row.options) string(option);
    }
  }
  if (dialog) {
    number(int(d.kind)); string(d.heading); string(d.text); number(d.focus); number(d.scroll); number(d.button);
    number(d.edit_selected); number(d.max_chars); number(d.key);
    auto strings = [&](const auto& values) { number(values.size()); for (const auto& value : values) string(value); };
    strings(d.rows); strings(d.dates); strings(d.details);
    number(d.edit.size()); key.append(reinterpret_cast<const char*>(d.edit.data()), d.edit.size() * sizeof(char16_t));
    number(d.keys.size()); key.append(reinterpret_cast<const char*>(d.keys.data()), d.keys.size() * sizeof(char16_t));
  }
  return key;
}

struct MacGfx {
  ID3D11Device* owner = nullptr;
  ID3D11Texture2D* texture = nullptr;
  ID3D11ShaderResourceView* view = nullptr;
  ID3D11VertexShader* vs = nullptr;
  ID3D11PixelShader* ps = nullptr;
  ID3D11SamplerState* sampler = nullptr;
  ID3D11BlendState* blend = nullptr;
  ID3D11DepthStencilState* depth = nullptr;
  ID3D11RasterizerState* raster = nullptr;
  std::string raster_key;
  bool ok = false;
  ~MacGfx() {
    mac_release(raster); mac_release(depth); mac_release(blend); mac_release(sampler);
    mac_release(ps); mac_release(vs); mac_release(view); mac_release(texture); mac_release(owner);
  }
  explicit MacGfx(ID3D11Device* dev, UINT width = UINT(kW), UINT height = UINT(kH)) : owner(dev) {
    owner->AddRef();
    static constexpr char vertex[] = R"(#include <metal_stdlib>
using namespace metal;
struct UIOut { float4 position [[position]]; float2 uv; };
vertex UIOut acfa_vs(uint id [[vertex_id]]) {
  float2 uv = float2(float((id << 1) & 2), float(id & 2));
  return {float4(uv * float2(2, -2) + float2(-1, 1), 0, 1), uv};
})";
    static constexpr char fragment[] = R"(#include <metal_stdlib>
using namespace metal;
struct UIOut { float4 position [[position]]; float2 uv; };
fragment float4 acfa_ps(UIOut in [[stage_in]], texture2d<float> image [[texture(0)]], sampler filtering [[sampler(0)]]) {
  return image.sample(filtering, in.uv);
})";
    D3D11_TEXTURE2D_DESC texture_desc{};
    texture_desc.Width = width; texture_desc.Height = height; texture_desc.MipLevels = 1; texture_desc.ArraySize = 1;
    texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; texture_desc.SampleDesc.Count = 1;
    texture_desc.Usage = D3D11_USAGE_DEFAULT; texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SAMPLER_DESC sampler_desc{};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = sampler_desc.AddressV = sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX; sampler_desc.MaxAnisotropy = 1;
    D3D11_BLEND_DESC blend_desc{};
    auto& target = blend_desc.RenderTarget[0]; target.BlendEnable = TRUE;
    target.SrcBlend = target.SrcBlendAlpha = D3D11_BLEND_ONE;
    target.DestBlend = target.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    target.BlendOp = target.BlendOpAlpha = D3D11_BLEND_OP_ADD; target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    D3D11_DEPTH_STENCIL_DESC depth_desc{};
    depth_desc.DepthFunc = D3D11_COMPARISON_ALWAYS; depth_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    D3D11_RASTERIZER_DESC raster_desc{};
    raster_desc.FillMode = D3D11_FILL_SOLID; raster_desc.CullMode = D3D11_CULL_NONE; raster_desc.DepthClipEnable = TRUE;
    ok = SUCCEEDED(dev->CreateTexture2D(&texture_desc, nullptr, &texture)) &&
         SUCCEEDED(dev->CreateShaderResourceView(texture, nullptr, &view)) &&
         SUCCEEDED(dev->CreateVertexShader(vertex, sizeof vertex - 1, nullptr, &vs)) &&
         SUCCEEDED(dev->CreatePixelShader(fragment, sizeof fragment - 1, nullptr, &ps)) &&
         SUCCEEDED(dev->CreateSamplerState(&sampler_desc, &sampler)) &&
         SUCCEEDED(dev->CreateBlendState(&blend_desc, &blend)) &&
         SUCCEEDED(dev->CreateDepthStencilState(&depth_desc, &depth)) &&
         SUCCEEDED(dev->CreateRasterizerState(&raster_desc, &raster));
    if (!ok) LOG_E("ui", "macOS overlay resources failed: %s", acfa::metal::last_error(dev).c_str());
  }
};
std::unique_ptr<MacGfx> g_mac_gfx;

// Preserve every binding changed by this overlay; the next RSX draw resumes its actual state.
struct MacSavedState {
  ID3D11DeviceContext* context;
  ID3D11RenderTargetView* targets[8]{}; ID3D11DepthStencilView* depth_view = nullptr;
  ID3D11BlendState* blend = nullptr; FLOAT factors[4]{}; UINT mask = 0;
  ID3D11DepthStencilState* depth = nullptr; UINT stencil = 0;
  ID3D11RasterizerState* raster = nullptr;
  UINT viewport_count = 16, scissor_count = 16;
  D3D11_VIEWPORT viewports[16]{}; D3D11_RECT scissors[16]{};
  ID3D11VertexShader* vs = nullptr; ID3D11PixelShader* ps = nullptr;
  ID3D11ShaderResourceView* resource = nullptr; ID3D11SamplerState* sampler = nullptr;
  D3D11_PRIMITIVE_TOPOLOGY topology{};
  explicit MacSavedState(ID3D11DeviceContext* ctx) : context(ctx) {
    ctx->OMGetRenderTargets(8, targets, &depth_view); ctx->OMGetBlendState(&blend, factors, &mask);
    ctx->OMGetDepthStencilState(&depth, &stencil); ctx->RSGetState(&raster);
    ctx->RSGetViewports(&viewport_count, viewports); ctx->RSGetScissorRects(&scissor_count, scissors);
    ctx->VSGetShader(&vs, nullptr, nullptr); ctx->PSGetShader(&ps, nullptr, nullptr);
    ctx->PSGetShaderResources(0, 1, &resource); ctx->PSGetSamplers(0, 1, &sampler); ctx->IAGetPrimitiveTopology(&topology);
  }
  ~MacSavedState() {
    context->PSSetShaderResources(0, 1, &resource); context->PSSetSamplers(0, 1, &sampler);
    context->VSSetShader(vs, nullptr, 0); context->PSSetShader(ps, nullptr, 0); context->IASetPrimitiveTopology(topology);
    context->RSSetViewports(viewport_count, viewports); context->RSSetScissorRects(scissor_count, scissors); context->RSSetState(raster);
    context->OMSetBlendState(blend, factors, mask); context->OMSetDepthStencilState(depth, stencil);
    context->OMSetRenderTargets(8, targets, depth_view);
    for (auto& target : targets) mac_release(target);
    mac_release(depth_view); mac_release(blend); mac_release(depth); mac_release(raster);
    mac_release(vs); mac_release(ps); mac_release(resource); mac_release(sampler);
  }
};

}  // namespace

void render(ID3D11Device* dev, ID3D11DeviceContext* context, ID3D11Texture2D* backbuffer, u32 width, u32 height) {
  if (!dev || !context || !backbuffer || !width || !height || !screen_up()) return;
  Snapshot snapshot;
  bool page = false;
  {
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    const auto state = g_state.load();
    if (state != State::Closed) {
      page = true; snapshot.closing = state == State::Closing; snapshot.rows = g_rows;
      snapshot.page = g_page; snapshot.focus = g_focus; snapshot.slot = g_slot; snapshot.scroll = g_scroll;
      snapshot.capturing = g_capturing;
      if (!g_toast.empty() && Clock::now() < g_toast_until) snapshot.toast = g_toast;
    }
  }
  if (snapshot.rows.empty() && !snapshot.closing) page = false;
  DialogView dialog;
  const bool have_dialog = sysdlg::view(&dialog);
  if (!page && !have_dialog) return;
  if (!g_mac_gfx || g_mac_gfx->owner != dev) g_mac_gfx = std::make_unique<MacGfx>(dev);
  auto& gfx = *g_mac_gfx;
  if (!gfx.ok) return;
  const auto key = mac_raster_key(page, snapshot, have_dialog, dialog);
  if (key != gfx.raster_key) {
    MacCanvas canvas;
    if (!canvas.cg) { LOG_E("ui", "macOS overlay bitmap allocation failed"); return; }
    if (page) mac_draw_page(canvas, snapshot);
    if (have_dialog) mac_draw_dialog(canvas, dialog);
    CGContextFlush(canvas.cg);
    context->UpdateSubresource(gfx.texture, 0, nullptr, canvas.pixels.data(), UINT(kW) * 4, 0);
    gfx.raster_key = key;
  }
  ID3D11RenderTargetView* target = nullptr;
  if (FAILED(dev->CreateRenderTargetView(backbuffer, nullptr, &target))) {
    LOG_E("ui", "macOS overlay backbuffer view failed: %s", acfa::metal::last_error(dev).c_str()); return;
  }
  {
    MacSavedState saved(context);
    const auto canvas = canvas_for(int(width), int(height));
    const D3D11_VIEWPORT viewport{canvas.ox, canvas.oy, kW * canvas.scale, kH * canvas.scale, 0, 1};
    context->OMSetRenderTargets(1, &target, nullptr); context->OMSetBlendState(gfx.blend, nullptr, UINT32_MAX);
    context->OMSetDepthStencilState(gfx.depth, 0); context->RSSetState(gfx.raster); context->RSSetViewports(1, &viewport);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(gfx.vs, nullptr, 0); context->PSSetShader(gfx.ps, nullptr, 0);
    context->PSSetShaderResources(0, 1, &gfx.view); context->PSSetSamplers(0, 1, &gfx.sampler); context->Draw(3, 0);
  }
  target->Release();
}

