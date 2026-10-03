// Included by every decompiled file (build/decomp/src/*.cpp).
#pragma once
#include "guest.h"
#include "gsyms.h"
#include "decl.h"

// a function the decompiler could not handle yet: logs and stops when called
void noma_unimplemented(const char* name);
// middleware the port leaves out (Flash, Mobiclip, ATOK, ...): logs once, returns 0
void noma_stub(const char* name, const char* region);
