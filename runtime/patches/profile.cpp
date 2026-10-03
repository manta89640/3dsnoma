// The family profile (the setup's and the options' birth year picker,
// layout::option::YearSelectWindow): years from profile_BirthYear_Min (1894) to
// profile_BirthYear_Max, which on the Wii is the server clock's current year. Here any year up to
// 2099 can be chosen. The year is kept as a u16; profile_BirthYear_IsValid, its only other check,
// is called without the "not in the future" test.
#include "noma_decomp.h"

namespace g {

u32 profile_BirthYear_Max(void) { return 2099; }

}  // namespace g
