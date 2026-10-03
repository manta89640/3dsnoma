// winid PID: print the CGWindowID of the largest on-screen window of a process (for
// `screencapture -l`); used by platform/3ds/run_azahar.sh to grab the emulator's window.
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv) {
    if (argc < 2)
        return 2;
    int pid = atoi(argv[1]);
    CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
    long best = -1;
    double best_area = 0;
    for (CFIndex i = 0; list && i < CFArrayGetCount(list); i++) {
        CFDictionaryRef w = CFArrayGetValueAtIndex(list, i);
        int owner = 0;
        long num = 0;
        CFNumberGetValue(CFDictionaryGetValue(w, kCGWindowOwnerPID), kCFNumberIntType, &owner);
        CFNumberGetValue(CFDictionaryGetValue(w, kCGWindowNumber), kCFNumberLongType, &num);
        CGRect r;
        CGRectMakeWithDictionaryRepresentation(CFDictionaryGetValue(w, kCGWindowBounds), &r);
        double area = r.size.width * r.size.height;
        if (owner == pid && area > best_area) {
            best = num;
            best_area = area;
        }
    }
    if (best < 0)
        return 1;
    printf("%ld\n", best);
    return 0;
}
