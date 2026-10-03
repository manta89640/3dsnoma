// Guest code address -> host function, for calls through function pointers (gcall).
#pragma once
struct NomaFn {
    unsigned addr;
    void* fn;
};
extern const NomaFn noma_functable[];
extern const unsigned noma_functable_count;
