// Unit tests for nm::allocateResources (qt/noisemaker/compiler/
// resources.{h,cpp}). Plain assert-style checks, no test framework
// dependency. Every case here mirrors a PARITY-CRITICAL note from
// reference/04 §1.3, hand-traced against the JS algorithm (resources.js,
// read in full) rather than the C++ port itself, to avoid "testing my own
// mistake twice."
//
// RED (before resources.cpp existed): this binary failed to link
// ("undefined symbols: nm::allocateResources"). GREEN: all checks below
// print PASS and the process exits 0.

#include "core/graph/resources.h"



#include <cstdio>

namespace {

int g_failures = 0;

void check(bool condition, const char* description) {
    if (condition) {
        std::printf("PASS: %s\n", description);
    } else {
        std::printf("FAIL: %s\n", description);
        ++g_failures;
    }
}

} // namespace

int main() {
    // ======================================================================
    // Basic linear chain: A -> B -> C. Each pass's single output can reuse
    // the PREVIOUS pass's now-dead input slot once it's released.
    // ======================================================================
    {
        nm::JsVector<nm::PassIO> passes;
        passes.append({{}, {nm::JsText(u"A")}});                              // pass 0: produce A
        passes.append({{nm::JsText(u"A")}, {nm::JsText(u"B")}});           // pass 1: A -> B
        passes.append({{nm::JsText(u"B")}, {nm::JsText(u"C")}});           // pass 2: B -> C
        const auto alloc = nm::allocateResources(passes);
        check(alloc.value(nm::JsText(u"A")) == nm::JsText(u"phys_0"), "linear chain: A gets phys_0");
        check(alloc.value(nm::JsText(u"B")) == nm::JsText(u"phys_1"),
              "linear chain: B gets phys_1 (A's slot isn't free until AFTER pass 1, so B can't reuse it "
              "at pass 1 itself -- self-aliasing rule)");
        check(alloc.value(nm::JsText(u"C")) == nm::JsText(u"phys_0"),
              "linear chain: C reuses phys_0 (A's slot, released at pass 1 since A's lifetime ends there, "
              "available for pass 2)");
    }

    // ======================================================================
    // Self-aliasing rule (reference/04 §1.3): a pass that reads AND writes
    // the SAME texId at the SAME index must NOT reuse its own slot there
    // (release happens strictly AFTER output allocation within one pass;
    // `availableAfter=i` is never `< i`).
    // ======================================================================
    {
        nm::JsVector<nm::PassIO> passes;
        passes.append({{}, {nm::JsText(u"X")}});                                    // pass 0: produce X
        passes.append({{nm::JsText(u"X")}, {nm::JsText(u"X")}});                 // pass 1: X -> X (in place)
        passes.append({{nm::JsText(u"X")}, {nm::JsText(u"Y")}});                 // pass 2: X -> Y
        const auto alloc = nm::allocateResources(passes);
        // X's lifetime spans passes 0..2 (last use at pass 2), so it is
        // NEVER released before pass 2 finishes -- Y cannot reuse X's slot
        // at pass 2 either (release happens after allocation within the
        // SAME pass). Y must get a fresh slot.
        check(alloc.value(nm::JsText(u"Y")) != alloc.value(nm::JsText(u"X")),
              "self-aliasing rule: X's slot is still live through pass 2 (its own last use), Y cannot alias it");
        check(alloc.value(nm::JsText(u"Y")) == nm::JsText(u"phys_1"), "Y gets a fresh slot (phys_1)");
    }

    // ======================================================================
    // Multi-output declaration order WITHIN one pass (reference/04 §1.3;
    // corpus-verified hazard -- points/physarum.json's agent pass declares
    // outputs {outXYZ,outVel,outRGBA} in exactly this non-alphabetical
    // order). Allocation must assign phys_N in the caller-supplied
    // PassIO::outputs ORDER, not alphabetically.
    // ======================================================================
    {
        nm::JsVector<nm::PassIO> passes;
        // A single pass with three outputs declared in a specific (non-
        // alphabetical) order, mirroring physarum's outXYZ/outVel/outRGBA.
        nm::PassIO p0;
        p0.outputs = {nm::JsText(u"outXYZ"), nm::JsText(u"outVel"), nm::JsText(u"outRGBA")};
        passes.append(p0);
        const auto alloc = nm::allocateResources(passes);
        check(alloc.value(nm::JsText(u"outXYZ")) == nm::JsText(u"phys_0"), "multi-output order: outXYZ (declared first) gets phys_0");
        check(alloc.value(nm::JsText(u"outVel")) == nm::JsText(u"phys_1"), "multi-output order: outVel (declared second) gets phys_1");
        check(alloc.value(nm::JsText(u"outRGBA")) == nm::JsText(u"phys_2"), "multi-output order: outRGBA (declared third) gets phys_2");
    }

    // ======================================================================
    // global_-prefixed ids are excluded from pooling entirely (infinite-
    // lived, no phys_N ever assigned).
    // ======================================================================
    {
        nm::JsVector<nm::PassIO> passes;
        passes.append({{}, {nm::JsText(u"global_o0"), nm::JsText(u"local_a")}});
        passes.append({{nm::JsText(u"global_o0"), nm::JsText(u"local_a")}, {nm::JsText(u"local_b")}});
        const auto alloc = nm::allocateResources(passes);
        check(!alloc.contains(nm::JsText(u"global_o0")), "global_-prefixed texId never receives a phys_N allocation");
        check(alloc.contains(nm::JsText(u"local_a")) && alloc.contains(nm::JsText(u"local_b")),
              "non-global texIds are pooled as usual alongside a global_ co-input");
    }

    // ======================================================================
    // Freelist reuse picks the FIRST-PUSHED eligible entry, not the
    // numerically-lowest physical id -- reference/04 §1.3's
    // `freeList.findIndex(item => item.availableAfter < i)` semantics.
    // Hand-traced (not just "run the port and see"):
    //   pass0: outputs=[A]              -> A=phys_0
    //   pass1: outputs=[B]              -> B=phys_1 (freeList still empty)
    //   pass2: inputs=[B,A] (B FIRST), outputs=[] -> both die here; release
    //          order follows INPUT-LIST order, so freeList becomes
    //          [{phys_1(B),availableAfter:2}, {phys_0(A),availableAfter:2}]
    //          -- B's entry precedes A's despite A having the LOWER phys_N.
    //   pass3: outputs=[C] -> findIndex picks the FIRST match (index 0,
    //          phys_1/B's old slot) even though phys_0 is numerically
    //          smaller -- proving push-order (not phys_N magnitude) wins.
    // ======================================================================
    {
        nm::JsVector<nm::PassIO> passes;
        passes.append({{}, {nm::JsText(u"A")}});
        passes.append({{}, {nm::JsText(u"B")}});
        passes.append({{nm::JsText(u"B"), nm::JsText(u"A")}, {}});
        passes.append({{}, {nm::JsText(u"C")}});
        const auto alloc = nm::allocateResources(passes);
        check(alloc.value(nm::JsText(u"A")) == nm::JsText(u"phys_0"), "freelist order setup: A gets phys_0");
        check(alloc.value(nm::JsText(u"B")) == nm::JsText(u"phys_1"), "freelist order setup: B gets phys_1");
        check(alloc.value(nm::JsText(u"C")) == nm::JsText(u"phys_1"),
              "C reuses phys_1 (B's slot, pushed FIRST because pass2's inputs list B before A), "
              "NOT phys_0 (A's slot, numerically lower but pushed SECOND) -- proves push-order priority");
    }

    if (g_failures == 0) {
        std::printf("ALL PASS (test_resources)\n");
        return 0;
    }
    std::printf("%d FAILURE(S) (test_resources)\n", g_failures);
    return 1;
}
