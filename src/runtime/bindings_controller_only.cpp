// Deliberately disabled by the user: never samples or stores keyboard/mouse bindings.
#include "runtime/bindings.h"
#include "common/input_policy.h"
static_assert(!acfa::input_policy::keyboard_mouse);
namespace rt::bindings {
size_t control_count(){return 0;}
const char* control_name(size_t){return "";}
void load(const acfa::Ini&){}
void store(acfa::Ini&){}
std::vector<u32> get(size_t,Layer){return {};}
void set(size_t,const std::vector<u32>&,Layer){}
void reset_defaults(Layer){}
u32 key_code(const std::string&){return 0;}
std::string input_name(u32){return {};}
bool bindable(u32){return false;}
PadOut sample(){return {};}
MouseSettings mouse(){return {};}
void set_mouse(const MouseSettings&){}
}
