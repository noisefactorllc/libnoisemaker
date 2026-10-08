// stroke_fill.cpp -- a stroke wider than 1 px, as Chromium's software
// canvas fills it: SkStroke turns the segment into a closed outline of lines
// and round-cap conics, and SkScan::AntiFillPath fills the outline with
// analytic anti-aliasing (SkScan_AAAPath.cpp), clipped to the canvas.
//
// The outline's convexity (SkPathPriv::ComputeConvexity) chooses the edge
// walker and the additive blitter: a small outline accumulates its coverage
// in an A8 mask that is blended once (MaskAdditiveBlitter), a larger one in
// run-length rows (RunBasedAdditiveBlitter, or SafeRLEAdditiveBlitter for an
// outline that is not convex). The mask takes outlines up to 32 px wide and
// 1024 bytes; the tracer's strokes, whose width and length scale with the
// canvas, exceed that only on canvases above about 7500 px. A stroke whose
// bounds reach coordinate 8192 takes Skia's non-anti-aliased
// SkScan::FillPath, which this file does not model; it fills such a stroke
// anti-aliased.
//
// A port of Skia; see the license notice in stroke_canvas.cpp.

#include "stroke_raster.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace nm::raster {

namespace {

Pt add(Pt a, Pt b) { return {a.x + b.x, a.y + b.y}; }
Pt sub(Pt a, Pt b) { return {a.x - b.x, a.y - b.y}; }
bool isFinite(Pt p) { return std::isfinite(p.x) && std::isfinite(p.y); }

// ---------------------------------------------------------------- path

enum class Verb { Move, Line, Conic, Close };

// An SkPathBuilder's points, verbs and conic weights.
struct Path {
    std::vector<Pt> pts;
    std::vector<Verb> verbs;
    std::vector<float> weights;

    void moveTo(Pt p) {
        pts.push_back(p);
        verbs.push_back(Verb::Move);
    }
    void lineTo(Pt p) {
        pts.push_back(p);
        verbs.push_back(Verb::Line);
    }
    // conicTo with the stroker's weight (finite, not 0 or 1).
    void conicTo(Pt p1, Pt p2, float w) {
        pts.push_back(p1);
        pts.push_back(p2);
        verbs.push_back(Verb::Conic);
        weights.push_back(w);
    }
    void close() { verbs.push_back(Verb::Close); }

    // computeFiniteBounds: the bounds of every point; false when one is not
    // finite or there are none.
    bool bounds(Rect& out) const {
        Rect r{std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(),
               -std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()};
        for (Pt p : pts) {
            if (!isFinite(p)) return false;
            r.left = std::fmin(r.left, p.x);
            r.top = std::fmin(r.top, p.y);
            r.right = std::fmax(r.right, p.x);
            r.bottom = std::fmax(r.bottom, p.y);
        }
        if (pts.empty()) return false;
        out = r;
        return true;
    }
};

// A path edge as SkPathEdgeIter returns it: each segment with its start
// point, and the implicit line that closes a contour.
struct PathEdge {
    bool conic = false;
    Pt pts[3];
    float w = 0.0f;
};

std::vector<PathEdge> pathEdges(const Path& path) {
    std::vector<PathEdge> out;
    std::size_t pi = 0;
    std::size_t wi = 0;
    std::size_t moveTo = 0;
    bool needsClose = false;
    const auto closeLine = [&](std::size_t last) {
        PathEdge e;
        e.pts[0] = path.pts[last];
        e.pts[1] = path.pts[moveTo];
        out.push_back(e);
    };
    for (Verb verb : path.verbs) {
        switch (verb) {
        case Verb::Move:
            if (needsClose) {
                closeLine(pi - 1);
                needsClose = false;
            }
            moveTo = pi;
            pi += 1;
            break;
        case Verb::Close:
            if (needsClose) {
                closeLine(pi - 1);
                needsClose = false;
            }
            break;
        case Verb::Line: {
            PathEdge e;
            e.pts[0] = path.pts[pi - 1];
            e.pts[1] = path.pts[pi];
            out.push_back(e);
            pi += 1;
            needsClose = true;
            break;
        }
        case Verb::Conic: {
            PathEdge e;
            e.conic = true;
            e.pts[0] = path.pts[pi - 1];
            e.pts[1] = path.pts[pi];
            e.pts[2] = path.pts[pi + 1];
            e.w = path.weights[wi];
            out.push_back(e);
            pi += 2;
            wi += 1;
            needsClose = true;
            break;
        }
        }
    }
    if (needsClose) closeLine(pi - 1);
    return out;
}

// --------------------------------------------------------------- stroker

constexpr float kRoot2Over2 = 0.70710677f; // SK_ScalarRoot2Over2

// RoundCapper: two quarter-circle conics around `pivot`.
void roundCap(Path& path, Pt pivot, Pt normal, Pt stop) {
    const Pt parallel{-normal.y, normal.x}; // RotateCW
    const Pt center = add(pivot, parallel);
    path.conicTo(add(center, normal), center, kRoot2Over2);
    path.conicTo(sub(center, normal), stop, kRoot2Over2);
}

// SkStroke::strokePath of moveTo(p0) lineTo(p1) with round caps (a single
// segment has no join) at `width` > 0: the outer line, the end cap, the
// reversed inner line and the start cap, closed.
Path strokeLine(Pt p0, Pt p1, float width) {
    const float radius = width / 2.0f;
    // set_normal_unitnormal: the unit vector rotated CCW, scaled by the
    // radius; round caps draw a zero-length segment upright.
    Pt unit;
    const Pt normal = normalize(p1.x - p0.x, p1.y - p0.y, unit) ? Pt{unit.y * radius, -unit.x * radius}
                                                              : Pt{radius, 0.0f};
    const Pt firstOuter = add(p0, normal);
    Path path;
    path.moveTo(firstOuter);
    path.lineTo(add(p1, normal));
    roundCap(path, p1, normal, sub(p1, normal));
    path.lineTo(sub(p0, normal));
    roundCap(path, p0, {-normal.x, -normal.y}, firstOuter);
    path.close();
    return path;
}

// ------------------------------------------------------------- convexity

enum class DirChange { Unknown, Left, Right, Straight, Backwards, Invalid };

// Convexicator.
struct Convexicator {
    Pt firstPt;
    Pt firstVec;
    Pt lastPt;
    Pt lastVec;
    DirChange expectedDir = DirChange::Invalid;
    bool firstDirectionKnown = false;
    int reversals = 0;

    void setMovePt(Pt p) {
        firstPt = p;
        lastPt = p;
        expectedDir = DirChange::Invalid;
    }

    bool addPt(Pt p) {
        if (lastPt == p) return true;
        if (firstPt == lastPt && expectedDir == DirChange::Invalid && lastVec == Pt{0.0f, 0.0f}) {
            lastVec = sub(p, lastPt);
            firstVec = lastVec;
        } else if (!addVec(sub(p, lastPt))) {
            return false;
        }
        lastPt = p;
        return true;
    }

    bool close() { return addPt(firstPt) && addVec(firstVec); }

    DirChange directionChange(Pt cur) const {
        const float cross = lastVec.x * cur.y - lastVec.y * cur.x;
        if (!std::isfinite(cross)) return DirChange::Unknown;
        if (cross == 0.0f) {
            const float dot = lastVec.x * cur.x + lastVec.y * cur.y;
            return dot < 0.0f ? DirChange::Backwards : DirChange::Straight;
        }
        return cross > 0.0f ? DirChange::Right : DirChange::Left;
    }

    bool addVec(Pt cur) {
        const DirChange dir = directionChange(cur);
        switch (dir) {
        case DirChange::Left:
        case DirChange::Right:
            if (expectedDir == DirChange::Invalid) {
                expectedDir = dir;
                firstDirectionKnown = true;
            } else if (dir != expectedDir) {
                firstDirectionKnown = false;
                return false;
            }
            lastVec = cur;
            return true;
        case DirChange::Straight: return true;
        case DirChange::Backwards:
            lastVec = cur;
            reversals += 1;
            return reversals < 3;
        case DirChange::Unknown:
        case DirChange::Invalid: return false;
        }
        return false;
    }

    // IsConcaveBySign: more than three sign changes of dx or dy.
    static bool isConcaveBySign(const std::vector<Pt>& points) {
        if (points.size() <= 3) return false;
        const auto sign = [](float x) { return x < 0.0f ? 1 : 0; };
        Pt curr = points[0];
        const Pt first = curr;
        int dxes = 0;
        int dyes = 0;
        int lastSx = 2;
        int lastSy = 2;
        // The points after the first, then the closing vector to the first.
        for (std::size_t i = 1; i <= points.size(); ++i) {
            const Pt next = i < points.size() ? points[i] : first;
            const Pt vec = sub(next, curr);
            if (vec != Pt{0.0f, 0.0f}) {
                if (!isFinite(vec)) return true;
                const int sx = sign(vec.x);
                const int sy = sign(vec.y);
                dxes += sx != lastSx ? 1 : 0;
                dyes += sy != lastSy ? 1 : 0;
                if (dxes > 3 || dyes > 3) return true;
                lastSx = sx;
                lastSy = sy;
            }
            curr = next;
        }
        return false;
    }
};

// SkPathPriv::ComputeConvexity(...) is convex (any direction or degenerate)
// for the stroker's single closed contour.
bool isConvex(const Path& path) {
    if (path.verbs.empty()) return true;
    if (Convexicator::isConcaveBySign(path.pts)) return false;
    Convexicator state;
    int contourCount = 0;
    bool needsClose = false;
    std::size_t pi = 0;
    for (Verb verb : path.verbs) {
        std::size_t newCount = 0;
        switch (verb) {
        case Verb::Move:
        case Verb::Line: newCount = 1; break;
        case Verb::Conic: newCount = 2; break;
        case Verb::Close: newCount = 0; break;
        }
        if (contourCount == 0) {
            if (verb == Verb::Move) {
                state.setMovePt(path.pts[pi]);
            } else {
                contourCount += 1;
                needsClose = true;
            }
        }
        if (contourCount == 1) {
            if (verb == Verb::Close || verb == Verb::Move) {
                if (!state.close()) return false;
                needsClose = false;
                contourCount += 1;
            } else {
                for (std::size_t k = 0; k < newCount; ++k) {
                    if (!state.addPt(path.pts[pi + k])) return false;
                }
            }
        } else if (contourCount > 1 && verb != Verb::Move) {
            return false;
        }
        pi += newCount;
    }
    if (needsClose && !state.close()) return false;
    return !(!state.firstDirectionKnown && state.reversals >= 3);
}

// --------------------------------------------------------------- geometry

// valid_unit_divide.
bool validUnitDivide(float numer, float denom, float& out) {
    if (numer < 0.0f) {
        numer = -numer;
        denom = -denom;
    }
    if (denom == 0.0f || numer == 0.0f || numer >= denom) return false;
    const float r = numer / denom;
    if (std::isnan(r) || r == 0.0f) return false;
    out = r;
    return true;
}

// is_not_monotonic(a, b, c).
bool isNotMonotonic(float a, float b, float c) {
    const float ab = a - b;
    float bc = b - c;
    if (ab < 0.0f) bc = -bc;
    return ab == 0.0f || bc < 0.0f;
}

Pt interp(Pt a, Pt b, float t) { return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}; }

struct Quad {
    Pt p[3];
};

// SkChopQuadAt(src, dst, t).
void chopQuadAt(const Pt src[3], float t, Pt dst[5]) {
    const Pt p01 = interp(src[0], src[1], t);
    const Pt p12 = interp(src[1], src[2], t);
    dst[0] = src[0];
    dst[1] = p01;
    dst[2] = interp(p01, p12, t);
    dst[3] = p12;
    dst[4] = src[2];
}

float coord(Pt p, bool y) { return y ? p.y : p.x; }
void setCoord(Pt& p, bool y, float v) { (y ? p.y : p.x) = v; }

// SkChopQuadAtYExtrema (`y` true) or SkChopQuadAtXExtrema: one or two
// monotonic quads.
std::vector<Quad> chopQuadAtExtrema(const Quad& src, bool y) {
    const float a = coord(src.p[0], y);
    float b = coord(src.p[1], y);
    const float c = coord(src.p[2], y);
    if (isNotMonotonic(a, b, c)) {
        float t = 0.0f;
        if (validUnitDivide(a - b, a - b - b + c, t)) {
            Pt dst[5];
            chopQuadAt(src.p, t, dst);
            // flatten_double_quad_extrema
            const float mid = coord(dst[2], y);
            setCoord(dst[1], y, mid);
            setCoord(dst[3], y, mid);
            return {Quad{{dst[0], dst[1], dst[2]}}, Quad{{dst[2], dst[3], dst[4]}}};
        }
        b = std::fabs(a - b) < std::fabs(b - c) ? a : c;
    }
    Quad out = src;
    setCoord(out.p[1], y, b);
    return {out};
}

// An SkConic.
struct Conic {
    Pt pts[3];
    float w = 0.0f;

    // SkConic::chop, the SK_SUPPORT_LEGACY_CONIC_CHOP form Chromium builds.
    void chop(Conic dst[2]) const {
        const float scale = 1.0f / (1.0f + w);
        const float newW = std::sqrt(0.5f + w * 0.5f);
        const Pt p0 = pts[0], p1 = pts[1], p2 = pts[2];
        const Pt wp1{w * p1.x, w * p1.y};
        const auto m = [scale](float a, float b, float c) { return ((a + (b + b)) + c) * scale * 0.5f; };
        Pt mid{m(p0.x, wp1.x, p2.x), m(p0.y, wp1.y, p2.y)};
        if (!isFinite(mid)) {
            const double wD = w;
            const double w2 = wD * 2.0;
            const double scaleHalf = 1.0 / (1.0 + wD) * 0.5;
            mid = {static_cast<float>((static_cast<double>(p0.x) + w2 * static_cast<double>(p1.x)
                                       + static_cast<double>(p2.x)) * scaleHalf),
                   static_cast<float>((static_cast<double>(p0.y) + w2 * static_cast<double>(p1.y)
                                       + static_cast<double>(p2.y)) * scaleHalf)};
        }
        const Pt c0{(p0.x + wp1.x) * scale, (p0.y + wp1.y) * scale};
        const Pt c1{(wp1.x + p2.x) * scale, (wp1.y + p2.y) * scale};
        dst[0] = Conic{{p0, c0, mid}, newW};
        dst[1] = Conic{{mid, c1, p2}, newW};
    }

    // computeQuadPOW2(tol).
    std::uint32_t quadPow2(float tol) const {
        if (tol < 0.0f || !std::isfinite(tol) || !isFinite(pts[0]) || !isFinite(pts[1]) || !isFinite(pts[2])
            || w < 0.0f || !std::isfinite(w)) {
            return 0;
        }
        const float a = w - 1.0f;
        const float k = a / (4.0f * (2.0f + a));
        const float x = k * (pts[0].x - 2.0f * pts[1].x + pts[2].x);
        const float y = k * (pts[0].y - 2.0f * pts[1].y + pts[2].y);
        float error = std::sqrt(x * x + y * y);
        std::uint32_t pow2 = 0;
        while (pow2 < 5) {
            if (error <= tol) break;
            error *= 0.25f;
            pow2 += 1;
        }
        return pow2;
    }
};

bool between(float a, float b, float c) { return (a - b) * (c - b) <= 0.0f; }

// subdivide(src, pts, level): appends each quad's control and end point.
void subdivide(const Conic& src, std::vector<Pt>& out, std::uint32_t level) {
    if (level == 0) {
        out.push_back(src.pts[1]);
        out.push_back(src.pts[2]);
        return;
    }
    Conic dst[2];
    src.chop(dst);
    const float startY = src.pts[0].y;
    const float endY = src.pts[2].y;
    if (between(startY, src.pts[1].y, endY)) {
        const float midY = dst[0].pts[2].y;
        if (!between(startY, midY, endY)) {
            const float closer = std::fabs(midY - startY) < std::fabs(midY - endY) ? startY : endY;
            dst[0].pts[2].y = closer;
            dst[1].pts[0].y = closer;
        }
        if (!between(startY, dst[0].pts[1].y, dst[0].pts[2].y)) dst[0].pts[1].y = startY;
        if (!between(dst[1].pts[0].y, dst[1].pts[1].y, endY)) dst[1].pts[1].y = endY;
    }
    subdivide(dst[0], out, level - 1);
    subdivide(dst[1], out, level - 1);
}

// SkAutoConicToQuads::computeQuads(pts, w, tol): 1 + 2n points of n quads.
std::vector<Pt> conicToQuads(const Pt pts[3], float w, float tol) {
    if (w <= 0.0f) {
        const Pt mid{(pts[0].x + pts[2].x) * 0.5f, (pts[0].y + pts[2].y) * 0.5f};
        return {pts[0], mid, pts[2]};
    }
    const Conic conic{{pts[0], pts[1], pts[2]}, w};
    std::uint32_t pow2 = conic.quadPow2(tol);
    if (w < 0.0f || !std::isfinite(w)) pow2 = 0;
    std::vector<Pt> out{pts[0]};
    bool lines = false;
    if (pow2 == 5) {
        Conic dst[2];
        conic.chop(dst);
        const auto near = [](Pt a, Pt b) {
            const float dx = a.x - b.x;
            const float dy = a.y - b.y;
            return !(std::isfinite(dx) && std::isfinite(dy) && (dx != 0.0f || dy != 0.0f));
        };
        if (near(dst[0].pts[1], dst[0].pts[2]) && near(dst[1].pts[0], dst[1].pts[1])) {
            out.push_back(dst[0].pts[1]);
            out.push_back(dst[0].pts[1]);
            out.push_back(dst[0].pts[1]);
            out.push_back(dst[1].pts[2]);
            lines = true;
        }
    }
    if (!lines) subdivide(conic, out, pow2);
    bool finite = true;
    for (Pt p : out) finite = finite && isFinite(p);
    if (!finite) {
        for (std::size_t i = 1; i + 1 < out.size(); ++i) out[i] = pts[1];
    }
    return out;
}

// The quads of a conic edge, kConicTol 0.25.
std::vector<Quad> conicQuads(const Pt pts[3], float w) {
    const std::vector<Pt> q = conicToQuads(pts, w, 0.25f);
    std::vector<Quad> out;
    for (std::size_t i = 0; i < (q.size() - 1) / 2; ++i) out.push_back(Quad{{q[2 * i], q[2 * i + 1], q[2 * i + 2]}});
    return out;
}

// SkFindUnitQuadRoots: the first root in (0, 1).
bool firstUnitQuadRoot(float a, float b, float c, float& out) {
    if (a == 0.0f) return validUnitDivide(-c, b, out);
    const double dr = static_cast<double>(b) * static_cast<double>(b)
        - 4.0 * static_cast<double>(a) * static_cast<double>(c);
    if (dr < 0.0) return false;
    const float r = static_cast<float>(std::sqrt(dr));
    if (!std::isfinite(r)) return false;
    const float q = b < 0.0f ? -(b - r) / 2.0f : -(b + r) / 2.0f;
    float roots[2];
    int count = 0;
    if (validUnitDivide(q, a, roots[count])) ++count;
    if (validUnitDivide(c, q, roots[count])) ++count;
    if (count == 2 && roots[0] > roots[1]) std::swap(roots[0], roots[1]);
    if (count == 0) return false;
    out = roots[0];
    return true;
}

// ------------------------------------------------------------ edge clipper

// sect_clamp_with_vertical from SkLineClipper.
float clampSectWithVertical(const Pt src[2], float x) {
    const double y = sectWithVertical(src, x);
    return static_cast<float>(pinUnsorted(y, src[0].y, src[1].y));
}

// SkLineClipper::ClipLine: 0 to 3 segments wholly inside `clip` in X
// (vertical ones on its sides), as a polyline.
std::vector<Pt> clipLine(const Pt pts[2], const Rect& clip, bool canCullRight) {
    int i0 = pts[0].y < pts[1].y ? 0 : 1;
    int i1 = 1 - i0;
    if (pts[i1].y <= clip.top || pts[i0].y >= clip.bottom) return {};
    Pt tmp[2] = {pts[0], pts[1]};
    if (pts[i0].y < clip.top) tmp[i0] = {sectWithHorizontal(pts, clip.top), clip.top};
    if (tmp[i1].y > clip.bottom) tmp[i1] = {sectWithHorizontal(pts, clip.bottom), clip.bottom};
    bool reverse = false;
    if (pts[0].x < pts[1].x) {
        i0 = 0;
        i1 = 1;
    } else {
        i0 = 1;
        i1 = 0;
        reverse = true;
    }
    std::vector<Pt> result;
    if (tmp[i1].x <= clip.left) {
        tmp[0].x = clip.left;
        tmp[1].x = clip.left;
        result = {tmp[0], tmp[1]};
        reverse = false;
    } else if (tmp[i0].x >= clip.right) {
        if (canCullRight) return {};
        tmp[0].x = clip.right;
        tmp[1].x = clip.right;
        result = {tmp[0], tmp[1]};
        reverse = false;
    } else {
        if (tmp[i0].x < clip.left) {
            result.push_back({clip.left, tmp[i0].y});
            result.push_back({clip.left, clampSectWithVertical(tmp, clip.left)});
        } else {
            result.push_back(tmp[i0]);
        }
        if (tmp[i1].x > clip.right) {
            result.push_back({clip.right, clampSectWithVertical(tmp, clip.right)});
            result.push_back({clip.right, tmp[i1].y});
        } else {
            result.push_back(tmp[i1]);
        }
    }
    if (reverse) std::reverse(result.begin(), result.end());
    return result;
}

// A clipped edge: SkEdgeClipper's line and quad verbs.
struct ClippedEdge {
    bool quad = false;
    Pt pts[3];
};

bool chopMonoQuadAt(float c0, float c1, float c2, float target, float& t) {
    const float a = c0 - c1 - c1 + c2;
    const float b = 2.0f * (c1 - c0);
    const float c = c0 - target;
    return firstUnitQuadRoot(a, b, c, t);
}

// chop_quad_in_Y.
void chopQuadInY(Pt pts[3], const Rect& clip) {
    float t = 0.0f;
    if (pts[0].y < clip.top) {
        if (chopMonoQuadAt(pts[0].y, pts[1].y, pts[2].y, clip.top, t)) {
            Pt tmp[5];
            chopQuadAt(pts, t, tmp);
            tmp[2].y = clip.top;
            if (tmp[3].y < clip.top) tmp[3].y = clip.top;
            pts[0] = tmp[2];
            pts[1] = tmp[3];
        } else {
            for (int i = 0; i < 3; ++i) {
                if (pts[i].y < clip.top) pts[i].y = clip.top;
            }
        }
    }
    if (pts[2].y > clip.bottom) {
        if (chopMonoQuadAt(pts[0].y, pts[1].y, pts[2].y, clip.bottom, t)) {
            Pt tmp[5];
            chopQuadAt(pts, t, tmp);
            if (tmp[1].y > clip.bottom) tmp[1].y = clip.bottom;
            tmp[2].y = clip.bottom;
            pts[1] = tmp[1];
            pts[2] = tmp[2];
        } else {
            for (int i = 0; i < 3; ++i) {
                if (pts[i].y > clip.bottom) pts[i].y = clip.bottom;
            }
        }
    }
}

struct EdgeClipper {
    bool canCullRight = false;
    std::vector<ClippedEdge> out;

    void appendVLine(float x, float y0, float y1, bool reverse) {
        if (reverse) std::swap(y0, y1);
        ClippedEdge e;
        e.pts[0] = {x, y0};
        e.pts[1] = {x, y1};
        out.push_back(e);
    }

    void appendQuad(const Pt pts[3], bool reverse) {
        ClippedEdge e;
        e.quad = true;
        if (reverse) {
            e.pts[0] = pts[2];
            e.pts[1] = pts[1];
            e.pts[2] = pts[0];
        } else {
            e.pts[0] = pts[0];
            e.pts[1] = pts[1];
            e.pts[2] = pts[2];
        }
        out.push_back(e);
    }

    void clipLine(Pt p0, Pt p1, const Rect& clip) {
        const Pt pts[2] = {p0, p1};
        const std::vector<Pt> lines = raster::clipLine(pts, clip, canCullRight);
        for (std::size_t i = 0; i + 1 < lines.size(); ++i) {
            ClippedEdge e;
            e.pts[0] = lines[i];
            e.pts[1] = lines[i + 1];
            out.push_back(e);
        }
    }

    // clipMonoQuad of a quad monotonic in X and Y.
    void clipMonoQuad(const Pt src[3], const Rect& clip) {
        Pt pts[3];
        bool reverse = false;
        if (src[0].y > src[2].y) {
            pts[0] = src[2];
            pts[1] = src[1];
            pts[2] = src[0];
            reverse = true;
        } else {
            pts[0] = src[0];
            pts[1] = src[1];
            pts[2] = src[2];
        }
        if (pts[2].y <= clip.top || pts[0].y >= clip.bottom) return;
        chopQuadInY(pts, clip);
        if (pts[0].x > pts[2].x) {
            std::swap(pts[0], pts[2]);
            reverse = !reverse;
        }
        if (pts[2].x <= clip.left) {
            appendVLine(clip.left, pts[0].y, pts[2].y, reverse);
            return;
        }
        if (pts[0].x >= clip.right) {
            if (!canCullRight) appendVLine(clip.right, pts[0].y, pts[2].y, reverse);
            return;
        }
        float t = 0.0f;
        if (pts[0].x < clip.left) {
            if (chopMonoQuadAt(pts[0].x, pts[1].x, pts[2].x, clip.left, t)) {
                Pt tmp[5];
                chopQuadAt(pts, t, tmp);
                appendVLine(clip.left, tmp[0].y, tmp[2].y, reverse);
                tmp[2].x = clip.left;
                if (tmp[3].x < clip.left) tmp[3].x = clip.left;
                pts[0] = tmp[2];
                pts[1] = tmp[3];
            } else {
                appendVLine(clip.left, pts[0].y, pts[2].y, reverse);
                return;
            }
        }
        if (pts[2].x > clip.right) {
            if (chopMonoQuadAt(pts[0].x, pts[1].x, pts[2].x, clip.right, t)) {
                Pt tmp[5];
                chopQuadAt(pts, t, tmp);
                if (tmp[1].x > clip.right) tmp[1].x = clip.right;
                tmp[2].x = clip.right;
                appendQuad(tmp, reverse);
                appendVLine(clip.right, tmp[2].y, tmp[4].y, reverse);
            } else {
                pts[1].x = std::fmin(pts[1].x, clip.right);
                pts[2].x = std::fmin(pts[2].x, clip.right);
                appendQuad(pts, reverse);
            }
        } else {
            appendQuad(pts, reverse);
        }
    }

    // clipQuad.
    void clipQuad(const Quad& src, const Rect& clip) {
        float top = std::numeric_limits<float>::infinity();
        float bottom = -std::numeric_limits<float>::infinity();
        for (const Pt& p : src.p) {
            top = std::fmin(top, p.y);
            bottom = std::fmax(bottom, p.y);
        }
        if (top >= clip.bottom || bottom <= clip.top) return;
        for (const Quad& monoY : chopQuadAtExtrema(src, true)) {
            for (const Quad& monoX : chopQuadAtExtrema(monoY, false)) clipMonoQuad(monoX.p, clip);
        }
    }
};

// ------------------------------------------------------------ fixed point

using Fixed = std::int32_t;
using FDot6 = std::int32_t;

constexpr Fixed kFixed1 = 1 << 16;
constexpr Fixed kFixedHalf = 1 << 15;
constexpr std::int32_t kMaxS32 = std::numeric_limits<std::int32_t>::max();
constexpr std::int32_t kMinS32 = std::numeric_limits<std::int32_t>::min();
constexpr int kDefaultAccuracy = 2;

Fixed fixedMul(Fixed a, Fixed b) {
    return static_cast<std::int32_t>((static_cast<std::int64_t>(a) * static_cast<std::int64_t>(b)) >> 16);
}
std::int32_t fixedFloor(Fixed x) { return x >> 16; }
std::int32_t fixedCeil(Fixed x) { return wadd(x, kFixed1 - 1) >> 16; }
std::int32_t fixedRoundToInt(Fixed x) { return wadd(x, kFixedHalf) >> 16; }
Fixed fixedRoundToFixed(Fixed x) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(wadd(x, kFixedHalf)) & 0xFFFF0000u);
}
Fixed fixedCeilToFixed(Fixed x) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(wadd(x, kFixed1 - 1)) & 0xFFFF0000u);
}
Fixed fixedFloorToFixed(Fixed x) { return static_cast<std::int32_t>(static_cast<std::uint32_t>(x) & 0xFFFF0000u); }
Fixed intToFixed(std::int32_t n) { return shl(n, 16); }
FDot6 fixedToFDot6(Fixed x) { return x >> 10; }
Fixed fdot6ToFixed(FDot6 x) { return shl(x, 10); }

std::int32_t satAdd(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(
        std::clamp<std::int64_t>(static_cast<std::int64_t>(a) + b, kMinS32, kMaxS32));
}
std::int32_t satSub(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(
        std::clamp<std::int64_t>(static_cast<std::int64_t>(a) - b, kMinS32, kMaxS32));
}

// SkAnalyticEdge::SnapY.
Fixed snapY(Fixed y) {
    constexpr std::uint32_t acc = kDefaultAccuracy;
    return static_cast<std::int32_t>(
        ((static_cast<std::uint32_t>(y) + (static_cast<std::uint32_t>(kFixed1) >> (acc + 1))) >> (16 - acc))
        << (16 - acc));
}

// quick_inverse's table: -(65536 * 64) / (1024 - i) for i in 0..1024, and 0.
constexpr std::int32_t kInverseTable[1025] = {
    -4096, -4100, -4104, -4108, -4112, -4116, -4120, -4124, -4128, -4132, -4136, -4140,
    -4144, -4148, -4152, -4156, -4161, -4165, -4169, -4173, -4177, -4181, -4185, -4190,
    -4194, -4198, -4202, -4206, -4211, -4215, -4219, -4223, -4228, -4232, -4236, -4240,
    -4245, -4249, -4253, -4258, -4262, -4266, -4271, -4275, -4279, -4284, -4288, -4293,
    -4297, -4301, -4306, -4310, -4315, -4319, -4324, -4328, -4332, -4337, -4341, -4346,
    -4350, -4355, -4359, -4364, -4369, -4373, -4378, -4382, -4387, -4391, -4396, -4401,
    -4405, -4410, -4415, -4419, -4424, -4429, -4433, -4438, -4443, -4447, -4452, -4457,
    -4462, -4466, -4471, -4476, -4481, -4485, -4490, -4495, -4500, -4505, -4510, -4514,
    -4519, -4524, -4529, -4534, -4539, -4544, -4549, -4554, -4559, -4563, -4568, -4573,
    -4578, -4583, -4588, -4593, -4599, -4604, -4609, -4614, -4619, -4624, -4629, -4634,
    -4639, -4644, -4650, -4655, -4660, -4665, -4670, -4675, -4681, -4686, -4691, -4696,
    -4702, -4707, -4712, -4718, -4723, -4728, -4733, -4739, -4744, -4750, -4755, -4760,
    -4766, -4771, -4777, -4782, -4788, -4793, -4798, -4804, -4809, -4815, -4821, -4826,
    -4832, -4837, -4843, -4848, -4854, -4860, -4865, -4871, -4877, -4882, -4888, -4894,
    -4899, -4905, -4911, -4917, -4922, -4928, -4934, -4940, -4946, -4951, -4957, -4963,
    -4969, -4975, -4981, -4987, -4993, -4999, -5005, -5011, -5017, -5023, -5029, -5035,
    -5041, -5047, -5053, -5059, -5065, -5071, -5077, -5084, -5090, -5096, -5102, -5108,
    -5115, -5121, -5127, -5133, -5140, -5146, -5152, -5159, -5165, -5171, -5178, -5184,
    -5190, -5197, -5203, -5210, -5216, -5223, -5229, -5236, -5242, -5249, -5256, -5262,
    -5269, -5275, -5282, -5289, -5295, -5302, -5309, -5315, -5322, -5329, -5336, -5343,
    -5349, -5356, -5363, -5370, -5377, -5384, -5391, -5398, -5405, -5412, -5418, -5426,
    -5433, -5440, -5447, -5454, -5461, -5468, -5475, -5482, -5489, -5497, -5504, -5511,
    -5518, -5526, -5533, -5540, -5548, -5555, -5562, -5570, -5577, -5584, -5592, -5599,
    -5607, -5614, -5622, -5629, -5637, -5645, -5652, -5660, -5667, -5675, -5683, -5691,
    -5698, -5706, -5714, -5722, -5729, -5737, -5745, -5753, -5761, -5769, -5777, -5785,
    -5793, -5801, -5809, -5817, -5825, -5833, -5841, -5849, -5857, -5866, -5874, -5882,
    -5890, -5899, -5907, -5915, -5924, -5932, -5940, -5949, -5957, -5966, -5974, -5983,
    -5991, -6000, -6009, -6017, -6026, -6034, -6043, -6052, -6061, -6069, -6078, -6087,
    -6096, -6105, -6114, -6123, -6132, -6141, -6150, -6159, -6168, -6177, -6186, -6195,
    -6204, -6213, -6223, -6232, -6241, -6250, -6260, -6269, -6278, -6288, -6297, -6307,
    -6316, -6326, -6335, -6345, -6355, -6364, -6374, -6384, -6393, -6403, -6413, -6423,
    -6432, -6442, -6452, -6462, -6472, -6482, -6492, -6502, -6512, -6523, -6533, -6543,
    -6553, -6563, -6574, -6584, -6594, -6605, -6615, -6626, -6636, -6647, -6657, -6668,
    -6678, -6689, -6700, -6710, -6721, -6732, -6743, -6754, -6765, -6775, -6786, -6797,
    -6808, -6820, -6831, -6842, -6853, -6864, -6875, -6887, -6898, -6909, -6921, -6932,
    -6944, -6955, -6967, -6978, -6990, -7002, -7013, -7025, -7037, -7049, -7061, -7073,
    -7084, -7096, -7108, -7121, -7133, -7145, -7157, -7169, -7182, -7194, -7206, -7219,
    -7231, -7244, -7256, -7269, -7281, -7294, -7307, -7319, -7332, -7345, -7358, -7371,
    -7384, -7397, -7410, -7423, -7436, -7449, -7463, -7476, -7489, -7503, -7516, -7530,
    -7543, -7557, -7570, -7584, -7598, -7612, -7626, -7639, -7653, -7667, -7681, -7695,
    -7710, -7724, -7738, -7752, -7767, -7781, -7796, -7810, -7825, -7839, -7854, -7869,
    -7884, -7898, -7913, -7928, -7943, -7958, -7973, -7989, -8004, -8019, -8035, -8050,
    -8065, -8081, -8097, -8112, -8128, -8144, -8160, -8176, -8192, -8208, -8224, -8240,
    -8256, -8272, -8289, -8305, -8322, -8338, -8355, -8371, -8388, -8405, -8422, -8439,
    -8456, -8473, -8490, -8507, -8525, -8542, -8559, -8577, -8594, -8612, -8630, -8648,
    -8665, -8683, -8701, -8719, -8738, -8756, -8774, -8793, -8811, -8830, -8848, -8867,
    -8886, -8905, -8924, -8943, -8962, -8981, -9000, -9020, -9039, -9058, -9078, -9098,
    -9118, -9137, -9157, -9177, -9198, -9218, -9238, -9258, -9279, -9300, -9320, -9341,
    -9362, -9383, -9404, -9425, -9446, -9467, -9489, -9510, -9532, -9554, -9576, -9597,
    -9619, -9642, -9664, -9686, -9709, -9731, -9754, -9776, -9799, -9822, -9845, -9868,
    -9892, -9915, -9939, -9962, -9986, -10010, -10034, -10058, -10082, -10106, -10131, -10155,
    -10180, -10205, -10230, -10255, -10280, -10305, -10330, -10356, -10381, -10407, -10433, -10459,
    -10485, -10512, -10538, -10564, -10591, -10618, -10645, -10672, -10699, -10727, -10754, -10782,
    -10810, -10837, -10866, -10894, -10922, -10951, -10979, -11008, -11037, -11066, -11096, -11125,
    -11155, -11184, -11214, -11244, -11275, -11305, -11335, -11366, -11397, -11428, -11459, -11491,
    -11522, -11554, -11586, -11618, -11650, -11683, -11715, -11748, -11781, -11814, -11848, -11881,
    -11915, -11949, -11983, -12018, -12052, -12087, -12122, -12157, -12192, -12228, -12264, -12300,
    -12336, -12372, -12409, -12446, -12483, -12520, -12557, -12595, -12633, -12671, -12710, -12748,
    -12787, -12826, -12865, -12905, -12945, -12985, -13025, -13066, -13107, -13148, -13189, -13231,
    -13273, -13315, -13357, -13400, -13443, -13486, -13530, -13573, -13617, -13662, -13706, -13751,
    -13797, -13842, -13888, -13934, -13981, -14027, -14074, -14122, -14169, -14217, -14266, -14315,
    -14364, -14413, -14463, -14513, -14563, -14614, -14665, -14716, -14768, -14820, -14873, -14926,
    -14979, -15033, -15087, -15141, -15196, -15252, -15307, -15363, -15420, -15477, -15534, -15592,
    -15650, -15709, -15768, -15827, -15887, -15947, -16008, -16070, -16131, -16194, -16256, -16320,
    -16384, -16448, -16513, -16578, -16644, -16710, -16777, -16844, -16912, -16980, -17050, -17119,
    -17189, -17260, -17331, -17403, -17476, -17549, -17623, -17697, -17772, -17848, -17924, -18001,
    -18078, -18157, -18236, -18315, -18396, -18477, -18558, -18641, -18724, -18808, -18893, -18978,
    -19065, -19152, -19239, -19328, -19418, -19508, -19599, -19691, -19784, -19878, -19972, -20068,
    -20164, -20262, -20360, -20460, -20560, -20661, -20763, -20867, -20971, -21076, -21183, -21290,
    -21399, -21509, -21620, -21732, -21845, -21959, -22075, -22192, -22310, -22429, -22550, -22671,
    -22795, -22919, -23045, -23172, -23301, -23431, -23563, -23696, -23831, -23967, -24105, -24244,
    -24385, -24528, -24672, -24818, -24966, -25115, -25266, -25420, -25575, -25731, -25890, -26051,
    -26214, -26379, -26546, -26715, -26886, -27060, -27235, -27413, -27594, -27776, -27962, -28149,
    -28339, -28532, -28728, -28926, -29127, -29330, -29537, -29746, -29959, -30174, -30393, -30615,
    -30840, -31068, -31300, -31536, -31775, -32017, -32263, -32513, -32768, -33026, -33288, -33554,
    -33825, -34100, -34379, -34663, -34952, -35246, -35544, -35848, -36157, -36472, -36792, -37117,
    -37449, -37786, -38130, -38479, -38836, -39199, -39568, -39945, -40329, -40721, -41120, -41527,
    -41943, -42366, -42799, -43240, -43690, -44150, -44620, -45100, -45590, -46091, -46603, -47127,
    -47662, -48210, -48770, -49344, -49932, -50533, -51150, -51781, -52428, -53092, -53773, -54471,
    -55188, -55924, -56679, -57456, -58254, -59074, -59918, -60787, -61680, -62601, -63550, -64527,
    -65536, -66576, -67650, -68759, -69905, -71089, -72315, -73584, -74898, -76260, -77672, -79137,
    -80659, -82241, -83886, -85598, -87381, -89240, -91180, -93206, -95325, -97541, -99864, -102300,
    -104857, -107546, -110376, -113359, -116508, -119837, -123361, -127100, -131072, -135300, -139810, -144631,
    -149796, -155344, -161319, -167772, -174762, -182361, -190650, -199728, -209715, -220752, -233016, -246723,
    -262144, -279620, -299593, -322638, -349525, -381300, -419430, -466033, -524288, -599186, -699050, -838860,
    -1048576, -1398101, -2097152, -4194304, 0,
};

// quick_inverse(x) for |x| <= 1024.
Fixed quickInverse(FDot6 x) {
    return x > 0 ? -kInverseTable[1024 - x] : kInverseTable[1024 + x];
}

// SkFDot6Div.
Fixed fdot6Div(FDot6 a, FDot6 b) {
    if (static_cast<std::int32_t>(static_cast<std::int16_t>(a)) == a) return shl(a, 16) / b;
    return static_cast<std::int32_t>(
        std::clamp<std::int64_t>((static_cast<std::int64_t>(a) << 16) / b, kMinS32, kMaxS32));
}

// quick_div.
Fixed quickDiv(FDot6 a, FDot6 b) {
    constexpr std::int32_t kMinBits = 3;
    constexpr std::int32_t kMaxAbsA = 1 << (31 - (22 - kMinBits));
    const std::int32_t absA = wabs(a);
    const std::int32_t absB = wabs(b);
    if (absB >= (1 << kMinBits) && absB < 1024 && absA < kMaxAbsA) return wmul(a, quickInverse(b)) >> 6;
    return fdot6Div(a, b);
}

// ------------------------------------------------------- analytic edges

constexpr std::size_t kNone = static_cast<std::size_t>(-1);

// An SkAnalyticEdge (a line, or a quadratic's current line segment), linked
// by index.
struct Edge {
    std::size_t next = kNone;
    std::size_t prev = kNone;
    Fixed x = 0;
    Fixed dx = 0;
    Fixed upperX = 0;
    Fixed y = 0;
    Fixed upperY = 0;
    Fixed lowerY = 0;
    Fixed dy = 0;
    bool isLine = true;
    std::int32_t curveCount = 0;
    std::int32_t curveShift = 0;
    std::int32_t winding = 1;
    Fixed qx = 0;
    Fixed qy = 0;
    Fixed qdx = 0;
    Fixed qdy = 0;
    Fixed qddx = 0;
    Fixed qddy = 0;
    Fixed qlastX = 0;
    Fixed qlastY = 0;
    Fixed snappedX = 0;
    Fixed snappedY = 0;

    // fDY of a segment from its FDot6 deltas and slope.
    static Fixed inverseDy(FDot6 dxIn, FDot6 dyIn, Fixed slope, std::int32_t absSlope) {
        if (dxIn == 0 || slope == 0) return kMaxS32;
        if (absSlope < 1024) return quickInverse(absSlope);
        return wabs(quickDiv(dyIn, dxIn));
    }

    // setLine(p0, p1).
    static bool line(Pt p0, Pt p1, Edge& e) {
        const auto to = [](float v) { return fdot6ToFixed(toFDot6(v * 4.0f)) >> kDefaultAccuracy; };
        Fixed x0 = to(p0.x), y0 = snapY(to(p0.y));
        Fixed x1 = to(p1.x), y1 = snapY(to(p1.y));
        std::int32_t winding = 1;
        if (y0 > y1) {
            std::swap(x0, x1);
            std::swap(y0, y1);
            winding = -1;
        }
        const FDot6 dyD = fixedToFDot6(wsub(y1, y0));
        if (dyD == 0) return false;
        const FDot6 dxD = fixedToFDot6(wsub(x1, x0));
        const Fixed slope = quickDiv(dxD, dyD);
        e = Edge{};
        e.x = x0;
        e.dx = slope;
        e.upperX = x0;
        e.y = y0;
        e.upperY = y0;
        e.lowerY = y1;
        e.dy = inverseDy(dxD, dyD, slope, wabs(slope));
        e.isLine = true;
        e.winding = winding;
        return true;
    }

    // updateLine(x0, y0, x1, y1, slope).
    bool updateLine(Fixed x0, Fixed y0, Fixed x1, Fixed y1, Fixed slope) {
        if (y0 > y1) {
            std::swap(x0, x1);
            std::swap(y0, y1);
            winding = -winding;
        }
        const FDot6 dxD = fixedToFDot6(wsub(x1, x0));
        const FDot6 dyD = fixedToFDot6(wsub(y1, y0));
        if (dyD == 0) return false;
        const std::int32_t absSlope = wabs(fixedToFDot6(slope));
        x = x0;
        dx = slope;
        upperX = x0;
        y = y0;
        upperY = y0;
        lowerY = y1;
        dy = inverseDy(dxD, dyD, slope, absSlope);
        return true;
    }

    // setQuadratic(pts).
    static bool quad(const Pt pts[3], Edge& e) {
        const float scale = static_cast<float>(1 << (kDefaultAccuracy + 6));
        const auto fd = [scale](float v) { return toI32(static_cast<double>(v * scale)); };
        std::int32_t x0 = fd(pts[0].x), y0 = fd(pts[0].y);
        const std::int32_t x1 = fd(pts[1].x), y1 = fd(pts[1].y);
        std::int32_t x2 = fd(pts[2].x), y2 = fd(pts[2].y);
        std::int32_t winding = 1;
        if (y0 > y2) {
            std::swap(x0, x2);
            std::swap(y0, y2);
            winding = -1;
        }
        const std::int32_t top = wadd(y0, 32) >> 6;
        const std::int32_t bot = wadd(y2, 32) >> 6;
        if (top == bot) return false;
        std::int32_t shift = 0;
        {
            const std::int32_t ddx = wsub(wsub(shl(x1, 1), x0), x2) >> 2;
            const std::int32_t ddy = wsub(wsub(shl(y1, 1), y0), y2) >> 2;
            // diff_to_shift(dx, dy, shiftAA = 2)
            const std::int32_t adx = wabs(ddx);
            const std::int32_t ady = wabs(ddy);
            std::int32_t dist = adx > ady ? adx + (ady >> 1) : ady + (adx >> 1);
            dist = (dist + (1 << 4)) >> 5;
            shift = (32 - clz32(static_cast<std::uint32_t>(dist))) >> 1;
            shift = std::clamp(shift, 1, 6);
        }
        e = Edge{};
        e.winding = winding;
        e.isLine = false;
        e.curveCount = 1 << shift;
        e.curveShift = shift - 1;
        const auto div2 = [](std::int32_t v) { return shl(v, 9); };
        Fixed a = div2(wadd(wsub(wsub(x0, x1), x1), x2));
        Fixed b = fdot6ToFixed(wsub(x1, x0));
        e.qx = fdot6ToFixed(x0);
        e.qdx = wadd(b, a >> shift);
        e.qddx = a >> (shift - 1);
        a = div2(wadd(wsub(wsub(y0, y1), y1), y2));
        b = fdot6ToFixed(wsub(y1, y0));
        e.qy = fdot6ToFixed(y0);
        e.qdy = wadd(b, a >> shift);
        e.qddy = a >> (shift - 1);
        e.qlastX = fdot6ToFixed(x2);
        e.qlastY = fdot6ToFixed(y2);
        // setQuadratic: back from the AA scale, y snapped.
        for (Fixed* v : {&e.qx, &e.qy, &e.qdx, &e.qdy, &e.qddx, &e.qddy, &e.qlastX, &e.qlastY}) {
            *v >>= kDefaultAccuracy;
        }
        e.qy = snapY(e.qy);
        e.qlastY = snapY(e.qlastY);
        e.snappedX = e.qx;
        e.snappedY = e.qy;
        return e.updateQuadratic();
    }

    // updateQuadratic().
    bool updateQuadratic() {
        bool success = false;
        std::int32_t count = curveCount;
        Fixed oldx = qx;
        Fixed oldy = qy;
        Fixed ddxStep = qdx;
        Fixed ddyStep = qdy;
        const std::int32_t shift = curveShift;
        Fixed newx = 0, newy = 0, newSnappedX = 0, newSnappedY = 0;
        do {
            Fixed slope = 0;
            count -= 1;
            if (count > 0) {
                newx = wadd(oldx, ddxStep >> shift);
                newy = wadd(oldy, ddyStep >> shift);
                if (wabs(ddyStep >> shift) >= kFixed1 * 2
                    && (static_cast<std::int64_t>(wabs(ddyStep)) << 6) > static_cast<std::int64_t>(wabs(ddxStep))) {
                    const FDot6 diffY = fixedToFDot6(wsub(newy, snappedY));
                    slope = diffY != 0 ? quickDiv(fixedToFDot6(wsub(newx, snappedX)), diffY) : kMaxS32;
                    newSnappedY = std::min(qlastY, fixedRoundToFixed(newy));
                    newSnappedX = wsub(newx, fixedMul(slope, wsub(newy, newSnappedY)));
                } else {
                    newSnappedY = std::min(qlastY, snapY(newy));
                    newSnappedX = newx;
                    const FDot6 diffY = fixedToFDot6(wsub(newSnappedY, snappedY));
                    slope = diffY != 0 ? quickDiv(fixedToFDot6(wsub(newx, snappedX)), diffY) : kMaxS32;
                }
                ddxStep = wadd(ddxStep, qddx);
                ddyStep = wadd(ddyStep, qddy);
            } else {
                newx = qlastX;
                newy = qlastY;
                newSnappedY = newy;
                newSnappedX = newx;
                const FDot6 diffY = fixedToFDot6(wsub(newy, snappedY));
                slope = diffY != 0 ? quickDiv(fixedToFDot6(wsub(newx, snappedX)), diffY) : kMaxS32;
            }
            if (slope < kMaxS32) success = updateLine(snappedX, snappedY, newSnappedX, newSnappedY, slope);
            oldx = newx;
            oldy = newy;
        } while (count > 0 && !success);
        qx = newx;
        qy = newy;
        qdx = ddxStep;
        qdy = ddyStep;
        snappedX = newSnappedX;
        snappedY = newSnappedY;
        curveCount = count;
        return success;
    }

    // update(last_y): the next segment of a quadratic; false for a line.
    bool update() { return curveCount > 0 && updateQuadratic(); }

    // goY(y).
    void goY(Fixed yIn) {
        if (yIn == wadd(y, kFixed1)) {
            x = wadd(x, dx);
            y = yIn;
        } else if (yIn != y) {
            x = wadd(upperX, fixedMul(dx, wsub(yIn, upperY)));
            y = yIn;
        }
    }

    // goY(y, yShift).
    void goYShift(Fixed yIn, std::int32_t yShift) {
        y = yIn;
        x = wadd(x, dx >> yShift);
    }

    // keepContinuous() of a quadratic.
    void keepContinuous() {
        snappedX = x;
        snappedY = y;
    }
};

enum class Combine { No, Partial, Total };

// SkAnalyticEdgeBuilder::combineVertical(edge, last).
Combine combineVertical(const Edge& edge, Edge& last) {
    const auto approx = [](Fixed a, Fixed b) { return wabs(wsub(a, b)) < 0x100; };
    if (!last.isLine || last.dx != 0 || edge.x != last.x) return Combine::No;
    if (edge.winding == last.winding) {
        if (edge.lowerY == last.upperY) {
            last.upperY = edge.upperY;
            last.y = last.upperY;
            return Combine::Partial;
        }
        if (approx(edge.upperY, last.lowerY)) {
            last.lowerY = edge.lowerY;
            return Combine::Partial;
        }
        return Combine::No;
    }
    if (approx(edge.upperY, last.upperY)) {
        if (approx(edge.lowerY, last.lowerY)) return Combine::Total;
        if (edge.lowerY < last.lowerY) {
            last.upperY = edge.lowerY;
            last.y = last.upperY;
            return Combine::Partial;
        }
        last.upperY = last.lowerY;
        last.y = last.upperY;
        last.lowerY = edge.lowerY;
        last.winding = edge.winding;
        return Combine::Partial;
    }
    if (approx(edge.lowerY, last.lowerY)) {
        if (edge.upperY > last.upperY) {
            last.lowerY = edge.upperY;
            return Combine::Partial;
        }
        last.lowerY = last.upperY;
        last.upperY = edge.upperY;
        last.y = last.upperY;
        last.winding = edge.winding;
        return Combine::Partial;
    }
    return Combine::No;
}

// SkAnalyticEdgeBuilder: the edges in path order.
struct EdgeBuilder {
    std::vector<Edge> list;

    // addLine, merging a vertical line into the previous one
    // (combineVertical).
    void addLine(Pt p0, Pt p1) {
        Edge edge;
        if (!Edge::line(p0, p1, edge)) return;
        if (edge.dx == 0 && edge.isLine && !list.empty()) {
            switch (combineVertical(edge, list.back())) {
            case Combine::Total: list.pop_back(); break;
            case Combine::Partial: break;
            case Combine::No: list.push_back(edge); break;
            }
        } else {
            list.push_back(edge);
        }
    }

    void addQuad(const Pt pts[3]) {
        Edge edge;
        if (Edge::quad(pts, edge)) list.push_back(edge);
    }

    // buildEdges(path, clip); `clip` null for none.
    static std::vector<Edge> build(const Path& path, const IRect* clip, bool canCullRight) {
        EdgeBuilder b;
        if (!clip) {
            for (const PathEdge& edge : pathEdges(path)) {
                if (!edge.conic) {
                    b.addLine(edge.pts[0], edge.pts[1]);
                    continue;
                }
                for (const Quad& quad : conicQuads(edge.pts, edge.w)) {
                    for (const Quad& mono : chopQuadAtExtrema(quad, true)) b.addQuad(mono.p);
                }
            }
            return b.list;
        }
        const Rect clipRect{static_cast<float>(clip->left), static_cast<float>(clip->top),
                            static_cast<float>(clip->right), static_cast<float>(clip->bottom)};
        EdgeClipper clipper;
        clipper.canCullRight = canCullRight;
        for (const PathEdge& edge : pathEdges(path)) {
            if (!edge.conic) {
                clipper.clipLine(edge.pts[0], edge.pts[1], clipRect);
                continue;
            }
            for (const Quad& quad : conicQuads(edge.pts, edge.w)) clipper.clipQuad(quad, clipRect);
        }
        for (const ClippedEdge& e : clipper.out) {
            if (!e.quad) {
                if (!isFinite(e.pts[0]) || !isFinite(e.pts[1])) return {};
                b.addLine(e.pts[0], e.pts[1]);
            } else {
                if (!isFinite(e.pts[0]) || !isFinite(e.pts[1]) || !isFinite(e.pts[2])) return {};
                b.addQuad(e.pts);
            }
        }
        return b.list;
    }
};

// compare_edges.
bool edgeLess(const Edge& a, const Edge& b) {
    if (a.upperY != b.upperY) return a.upperY < b.upperY;
    if (a.x != b.x) return a.x < b.x;
    return a.dx < b.dx;
}

void insertionSort(Edge* list, std::size_t n) {
    for (std::size_t next = 1; next < n; ++next) {
        if (!edgeLess(list[next], list[next - 1])) continue;
        const Edge insert = list[next];
        std::size_t hole = next;
        do {
            list[hole] = list[hole - 1];
            hole -= 1;
        } while (hole > 0 && edgeLess(insert, list[hole - 1]));
        list[hole] = insert;
    }
}

std::size_t partition(Edge* list, std::size_t n, std::size_t pivot) {
    const std::size_t right = n - 1;
    const Edge pivotValue = list[pivot];
    std::swap(list[pivot], list[right]);
    std::size_t newPivot = 0;
    for (std::size_t left = 0; left < right; ++left) {
        if (edgeLess(list[left], pivotValue)) {
            std::swap(list[left], list[newPivot]);
            newPivot += 1;
        }
    }
    std::swap(list[newPivot], list[right]);
    return newPivot;
}

void heapSort(Edge* a, std::size_t count) {
    const auto siftDown = [a](std::size_t root, std::size_t bottom) {
        const Edge x = a[root - 1];
        std::size_t child = root << 1;
        while (child <= bottom) {
            if (child < bottom && edgeLess(a[child - 1], a[child])) child += 1;
            if (edgeLess(x, a[child - 1])) {
                a[root - 1] = a[child - 1];
                root = child;
                child = root << 1;
            } else {
                break;
            }
        }
        a[root - 1] = x;
    };
    const auto siftUp = [a](std::size_t root, std::size_t bottom) {
        const Edge x = a[root - 1];
        const std::size_t start = root;
        std::size_t j = root << 1;
        while (j <= bottom) {
            if (j < bottom && edgeLess(a[j - 1], a[j])) j += 1;
            a[root - 1] = a[j - 1];
            root = j;
            j = root << 1;
        }
        j = root >> 1;
        while (j >= start) {
            if (edgeLess(a[j - 1], x)) {
                a[root - 1] = a[j - 1];
                root = j;
                j = root >> 1;
            } else {
                break;
            }
        }
        a[root - 1] = x;
    };
    for (std::size_t i = count >> 1; i >= 1; --i) siftDown(i, count);
    for (std::size_t i = count - 1; i >= 1; --i) {
        std::swap(a[0], a[i]);
        siftUp(1, i);
    }
}

void introSort(int depth, Edge* list, std::size_t count) {
    for (;;) {
        if (count <= 32) {
            insertionSort(list, count);
            return;
        }
        if (depth == 0) {
            heapSort(list, count);
            return;
        }
        depth -= 1;
        const std::size_t middle = (count - 1) >> 1;
        const std::size_t pivot = partition(list, count, middle);
        introSort(depth, list, pivot);
        list += pivot + 1;
        count -= pivot + 1;
    }
}

// SkTQSort with compare_edges.
void sortEdges(std::vector<Edge>& list) {
    const std::size_t n = list.size();
    if (n <= 1) return;
    const int depth = 2 * (64 - clz64(static_cast<std::uint64_t>(n - 2)));
    introSort(depth, list.data(), n);
}

// -------------------------------------------------------- additive blitters

std::uint8_t catchOverflow(std::uint32_t alpha) { return static_cast<std::uint8_t>(alpha - (alpha >> 8)); }

std::uint8_t safelyAdd(std::uint8_t a, std::uint8_t delta) {
    return static_cast<std::uint8_t>(std::min<std::uint32_t>(static_cast<std::uint32_t>(a) + delta, 0xFF));
}

std::uint8_t saturatingSub(std::uint8_t a, std::uint8_t b) { return a > b ? static_cast<std::uint8_t>(a - b) : 0; }

constexpr std::int32_t kMaskMaxWidth = 32;
constexpr std::int64_t kMaskMaxStorage = 1024;

// MaskAdditiveBlitter: coverage accumulated in an A8 mask over the path's
// bounds, blended once at the end.
struct MaskAdditive {
    Mask mask;
    IRect clipRect;

    static bool canHandle(const IRect& ir) {
        const std::int32_t width = ir.right - ir.left;
        if (width > kMaskMaxWidth) return false;
        const std::int64_t rb = (width + 3) & ~3;
        return rb * static_cast<std::int64_t>(ir.bottom - ir.top) <= kMaskMaxStorage;
    }

    void init(const IRect& ir, const IRect& clip) {
        // fStorage: (kMAX_STORAGE >> 2) + 2 words, the image one byte in.
        mask.image.assign((static_cast<std::size_t>(kMaskMaxStorage) >> 2 << 2) + 8, 0);
        mask.offset = 1;
        mask.bounds = ir;
        mask.rowBytes = static_cast<std::size_t>(ir.right - ir.left);
        if (!ir.intersect(clip, clipRect)) clipRect = IRect{};
    }

    bool index(std::int32_t x, std::int32_t y, std::size_t& out) const {
        const std::ptrdiff_t i = static_cast<std::ptrdiff_t>(mask.offset)
            + static_cast<std::ptrdiff_t>(y - mask.bounds.top) * static_cast<std::ptrdiff_t>(mask.rowBytes)
            + static_cast<std::ptrdiff_t>(x - mask.bounds.left);
        if (i < 0 || i >= static_cast<std::ptrdiff_t>(mask.image.size())) return false;
        out = static_cast<std::size_t>(i);
        return true;
    }

    template <typename F>
    void update(std::int32_t x, std::int32_t y, F f) {
        std::size_t i = 0;
        if (index(x, y, i)) mask.image[i] = f(mask.image[i]);
    }
};

// RunBasedAdditiveBlitter (`safe` false) or SafeRLEAdditiveBlitter: one row
// of accumulated coverage, flushed to the real blitter when the row changes.
struct RunAdditive {
    Blitter* real = nullptr;
    std::int32_t left = 0;
    std::int32_t width = 0;
    std::int32_t top = 0;
    std::int32_t currY = 0;
    std::vector<std::uint8_t> alpha;
    bool safe = false;

    void init(Blitter& realBlitter, const IRect& ir, const IRect& clip, bool safeAdd) {
        IRect sect;
        if (!ir.intersect(clip, sect)) sect = IRect{};
        real = &realBlitter;
        width = sect.right - sect.left;
        left = sect.left;
        top = sect.top;
        currY = sect.top - 1;
        alpha.assign(static_cast<std::size_t>(std::max(width, 0)), 0);
        safe = safeAdd;
    }

    std::uint8_t add(std::uint8_t a, std::uint8_t delta) const {
        return safe ? safelyAdd(a, delta) : catchOverflow(static_cast<std::uint32_t>(a) + delta);
    }

    void flush() {
        if (currY >= top) {
            // snapAlpha: values near 0 and 255 blit as 0 and 255.
            bool any = false;
            for (std::uint8_t& a : alpha) {
                a = a > 247 ? 0xFF : a < 8 ? 0 : a;
                any = any || a != 0;
            }
            if (any) {
                real->blitAntiH(left, currY, alpha.data(), static_cast<std::int32_t>(alpha.size()));
                std::fill(alpha.begin(), alpha.end(), std::uint8_t{0});
            }
            currY = top - 1;
        }
    }

    void checkY(std::int32_t y) {
        if (y != currY) {
            flush();
            currY = y;
        }
    }

    bool check(std::int32_t x, std::int32_t w) const { return x >= 0 && x + w <= width; }
};

// The additive blitter AAA accumulates into.
struct Additive {
    bool maskMode = false;
    MaskAdditive m;
    RunAdditive r;

    bool isMask() const { return maskMode; }

    // blitAntiH(x, y, antialias, len).
    void blitAntiHAlphas(std::int32_t x, std::int32_t y, const std::uint8_t* aa, std::int32_t count) {
        // The mask blitter's coverage is written directly; only the
        // run-length blitters take this call.
        if (maskMode) return;
        r.checkY(y);
        x -= r.left;
        if (x < 0) {
            aa += -x;
            count -= -x;
            x = 0;
        }
        const std::int32_t len = std::min(count, r.width - x);
        for (std::int32_t i = 0; i < len; ++i) {
            const std::size_t idx = static_cast<std::size_t>(x + i);
            r.alpha[idx] = r.add(r.alpha[idx], aa[i]);
        }
    }

    // blitAntiH(x, y, width, alpha).
    void blitAntiHN(std::int32_t x, std::int32_t y, std::int32_t width, std::uint8_t alpha) {
        if (maskMode) {
            for (std::int32_t i = 0; i < width; ++i) {
                m.update(x + i, y, [alpha](std::uint8_t a) {
                    return catchOverflow(static_cast<std::uint32_t>(a) + alpha);
                });
            }
            return;
        }
        r.checkY(y);
        x -= r.left;
        if (r.check(x, width)) {
            for (std::int32_t i = 0; i < width; ++i) {
                const std::size_t idx = static_cast<std::size_t>(x + i);
                r.alpha[idx] = r.add(r.alpha[idx], alpha);
            }
        }
    }

    // blitAntiH(x, y, alpha).
    void blitAntiH1(std::int32_t x, std::int32_t y, std::uint8_t alpha) { blitAntiHN(x, y, 1, alpha); }

    void flushIfYChanged(Fixed y, Fixed nextY) {
        if (!maskMode && fixedFloor(y) != fixedFloor(nextY)) r.flush();
    }

    // getRealBlitter(): the mask itself for the mask blitter (its blits set
    // coverage), the canvas blitter for the run-length ones.

    void realBlitV(std::int32_t x, std::int32_t y, std::int32_t height, std::uint8_t alpha) {
        if (!maskMode) {
            r.real->blitV(x, y, height, alpha);
            return;
        }
        if (alpha == 0) return;
        for (std::int32_t i = 0; i < height; ++i) m.update(x, y + i, [alpha](std::uint8_t) { return alpha; });
    }

    void realBlitRect(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height) {
        if (!maskMode) {
            r.real->blitRect(x, y, width, height);
            return;
        }
        for (std::int32_t row = 0; row < height; ++row) {
            for (std::int32_t i = 0; i < width; ++i) {
                m.update(x + i, y + row, [](std::uint8_t) { return std::uint8_t{0xFF}; });
            }
        }
    }

    void realBlitAntiRect(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height, std::uint8_t l,
                          std::uint8_t rAlpha) {
        if (!maskMode) {
            r.real->blitAntiRect(x, y, width, height, l, rAlpha);
            return;
        }
        realBlitV(x, y, height, l);
        realBlitV(x + 1 + width, y, height, rAlpha);
        realBlitRect(x + 1, y, width, height);
    }

    // AdditiveBlitter::blitH draws nothing; only the RLE blitters reach it.
    void realBlitH(std::int32_t x, std::int32_t y, std::int32_t width) {
        if (!maskMode) r.real->blitH(x, y, width);
    }

    void realBlitAntiH2(std::int32_t x, std::int32_t y, std::uint8_t a0, std::uint8_t a1) {
        if (!maskMode) r.real->blitAntiH2(x, y, a0, a1);
    }

    void realBlitAntiH(std::int32_t x, std::int32_t y, const std::uint8_t* aa, std::int32_t count) {
        if (!maskMode) r.real->blitAntiH(x, y, aa, count);
    }

    void maskSet(std::int32_t x, std::int32_t y, std::uint8_t a) {
        if (maskMode) m.update(x, y, [a](std::uint8_t) { return a; });
    }

    void maskSafelyAdd(std::int32_t x, std::int32_t y, std::uint8_t a) {
        if (maskMode) m.update(x, y, [a](std::uint8_t v) { return safelyAdd(v, a); });
    }
};

// ------------------------------------------------------- coverage helpers

std::uint8_t trapezoidToAlpha(Fixed l1, Fixed l2) { return static_cast<std::uint8_t>((wadd(l1, l2) / 2) >> 8); }

std::uint8_t partialTriangleToAlpha(Fixed a, Fixed b) {
    const std::int32_t area = wmul(wmul(a >> 11, a >> 11), b >> 11);
    return static_cast<std::uint8_t>((area >> 8) & 0xFF);
}

// get_partial_alpha(SkAlpha, SkFixed).
std::uint8_t partialAlphaFixed(std::uint8_t alpha, Fixed partialHeight) {
    return static_cast<std::uint8_t>(fixedRoundToInt(wmul(static_cast<std::int32_t>(alpha), partialHeight)));
}

// get_partial_alpha(SkAlpha, SkAlpha).
std::uint8_t partialAlpha(std::uint8_t alpha, std::uint8_t full) {
    return static_cast<std::uint8_t>((static_cast<std::uint32_t>(alpha) * full) >> 8);
}

// fixed_to_alpha of SkScan_AAAPath.
std::uint8_t fixedToAlpha(Fixed f) { return partialAlphaFixed(0xFF, f); }

Fixed approximateIntersection(Fixed l1, Fixed r1, Fixed l2, Fixed r2) {
    if (l1 > r1) std::swap(l1, r1);
    if (l2 > r2) std::swap(l2, r2);
    return wadd(std::max(l1, l2), std::min(r1, r2)) / 2;
}

void computeAlphaAboveLine(std::uint8_t* alphas, Fixed l, Fixed r, Fixed dy, std::uint8_t full) {
    const std::int32_t rr = fixedCeil(r);
    if (rr == 0) return;
    if (rr == 1) {
        const std::uint8_t a = static_cast<std::uint8_t>(wsub(wsub(shl(rr, 17), l), r) >> 9);
        alphas[0] = partialAlpha(a, full);
        return;
    }
    const Fixed first = kFixed1 - l;
    const Fixed last = r - ((rr - 1) << 16);
    const Fixed firstH = fixedMul(first, dy);
    alphas[0] = static_cast<std::uint8_t>(fixedMul(first, firstH) >> 9);
    std::int32_t alpha16 = satAdd(firstH, dy >> 1);
    for (std::int32_t i = 1; i < rr - 1; ++i) {
        alphas[i] = static_cast<std::uint8_t>(alpha16 >> 8);
        alpha16 = satAdd(alpha16, dy);
    }
    alphas[rr - 1] = static_cast<std::uint8_t>(full - partialTriangleToAlpha(last, dy));
}

void computeAlphaBelowLine(std::uint8_t* alphas, Fixed l, Fixed r, Fixed dy, std::uint8_t full) {
    const std::int32_t rr = fixedCeil(r);
    if (rr == 0) return;
    if (rr == 1) {
        alphas[0] = partialAlpha(trapezoidToAlpha(l, r), full);
        return;
    }
    const Fixed first = kFixed1 - l;
    const Fixed last = r - ((rr - 1) << 16);
    const Fixed lastH = fixedMul(last, dy);
    alphas[rr - 1] = static_cast<std::uint8_t>(fixedMul(last, lastH) >> 9);
    std::int32_t alpha16 = satAdd(lastH, dy >> 1);
    for (std::int32_t i = rr - 2; i > 0; --i) {
        alphas[i] = static_cast<std::uint8_t>((alpha16 >> 8) & 0xFF);
        alpha16 = satAdd(alpha16, dy);
    }
    alphas[0] = static_cast<std::uint8_t>(full - partialTriangleToAlpha(first, dy));
}

struct Row {
    std::int32_t y = 0;
    bool hasMaskRow = false;
    std::int32_t maskRow = 0;
    std::uint8_t full = 0;
    bool noReal = false;
};

void blitSingleAlpha(Additive& b, const Row& row, std::int32_t x, std::uint8_t alpha) {
    if (row.hasMaskRow) {
        if (row.full == 0xFF && !row.noReal) {
            b.maskSet(x, row.maskRow, alpha);
        } else {
            b.maskSafelyAdd(x, row.maskRow, partialAlpha(alpha, row.full));
        }
        return;
    }
    if (row.full == 0xFF && !row.noReal) {
        b.realBlitV(x, row.y, 1, alpha);
    } else {
        b.blitAntiH1(x, row.y, partialAlpha(alpha, row.full));
    }
}

void blitTwoAlphas(Additive& b, const Row& row, std::int32_t x, std::uint8_t a1, std::uint8_t a2) {
    if (row.hasMaskRow) {
        b.maskSafelyAdd(x, row.maskRow, a1);
        b.maskSafelyAdd(x + 1, row.maskRow, a2);
        return;
    }
    if (row.full == 0xFF && !row.noReal) {
        b.realBlitAntiH2(x, row.y, a1, a2);
    } else {
        b.blitAntiH1(x, row.y, a1);
        b.blitAntiH1(x + 1, row.y, a2);
    }
}

void blitFullAlpha(Additive& b, const Row& row, std::int32_t x, std::int32_t len) {
    if (row.hasMaskRow) {
        for (std::int32_t i = 0; i < len; ++i) b.maskSafelyAdd(x + i, row.maskRow, row.full);
        return;
    }
    if (row.full == 0xFF && !row.noReal) {
        b.realBlitH(x, row.y, len);
    } else {
        b.blitAntiHN(x, row.y, len, row.full);
    }
}

void blitAaaTrapezoidRow(Additive& b, const Row& row, Fixed ul, Fixed ur, Fixed ll, Fixed lr, Fixed lDy, Fixed rDy) {
    const std::uint8_t full = row.full;
    const std::int32_t l = fixedFloor(ul);
    const std::int32_t r = fixedCeil(lr);
    const std::int32_t len = r - l;
    if (len == 1) {
        blitSingleAlpha(b, row, l, trapezoidToAlpha(wsub(ur, ul), wsub(lr, ll)));
        return;
    }
    const std::size_t n = static_cast<std::size_t>(std::max(len, 0));
    std::vector<std::uint8_t> alphas(n, full);
    std::vector<std::uint8_t> temp(n + 1, 0);

    const std::int32_t uL = fixedFloor(ul);
    const std::int32_t lL = fixedCeil(ll);
    if (uL + 2 == lL) {
        const Fixed first = intToFixed(uL) + kFixed1 - ul;
        const Fixed second = ll - ul - first;
        const std::uint8_t a1 = static_cast<std::uint8_t>(full - partialTriangleToAlpha(first, lDy));
        const std::uint8_t a2 = partialTriangleToAlpha(second, lDy);
        alphas[0] = saturatingSub(alphas[0], a1);
        alphas[1] = saturatingSub(alphas[1], a2);
    } else {
        const std::size_t off = static_cast<std::size_t>(uL - l);
        computeAlphaBelowLine(&temp[off], ul - intToFixed(uL), ll - intToFixed(uL), lDy, full);
        for (std::int32_t i = uL; i < lL; ++i) {
            const std::size_t k = static_cast<std::size_t>(i - l);
            alphas[k] = saturatingSub(alphas[k], temp[k]);
        }
    }

    const std::int32_t uR = fixedFloor(ur);
    const std::int32_t lR = fixedCeil(lr);
    if (uR + 2 == lR) {
        const Fixed first = intToFixed(uR) + kFixed1 - ur;
        const Fixed second = lr - ur - first;
        const std::uint8_t a1 = partialTriangleToAlpha(first, rDy);
        const std::uint8_t a2 = static_cast<std::uint8_t>(full - partialTriangleToAlpha(second, rDy));
        alphas[n - 2] = saturatingSub(alphas[n - 2], a1);
        alphas[n - 1] = saturatingSub(alphas[n - 1], a2);
    } else {
        const std::size_t off = static_cast<std::size_t>(uR - l);
        computeAlphaAboveLine(&temp[off], ur - intToFixed(uR), lr - intToFixed(uR), rDy, full);
        for (std::int32_t i = uR; i < lR; ++i) {
            const std::size_t k = static_cast<std::size_t>(i - l);
            alphas[k] = saturatingSub(alphas[k], temp[k]);
        }
    }

    if (row.hasMaskRow) {
        for (std::size_t i = 0; i < n; ++i) b.maskSafelyAdd(l + static_cast<std::int32_t>(i), row.maskRow, alphas[i]);
        return;
    }
    if (full == 0xFF && !row.noReal) {
        b.realBlitAntiH(l, row.y, alphas.data(), static_cast<std::int32_t>(n));
    } else {
        b.blitAntiHAlphas(l, row.y, alphas.data(), static_cast<std::int32_t>(n));
    }
}

void blitTrapezoidRow(Additive& b, const Row& row, Fixed ul, Fixed ur, Fixed ll, Fixed lr, Fixed lDy, Fixed rDy) {
    if (ul > ur) return;
    if (ll > lr) {
        const Fixed x = approximateIntersection(ul, ll, ur, lr);
        ll = x;
        lr = x;
    }
    if (ul == ur && ll == lr) return;
    if (ul > ll) std::swap(ul, ll);
    if (ur > lr) std::swap(ur, lr);
    const std::uint8_t full = row.full;
    const Fixed joinLeft = fixedCeilToFixed(ll);
    const Fixed joinRite = fixedFloorToFixed(ur);
    if (joinLeft > joinRite) {
        blitAaaTrapezoidRow(b, row, ul, ur, ll, lr, lDy, rDy);
        return;
    }
    if (ul < joinLeft) {
        const std::int32_t len = fixedCeil(joinLeft - ul);
        if (len == 1) {
            blitSingleAlpha(b, row, ul >> 16, trapezoidToAlpha(joinLeft - ul, joinLeft - ll));
        } else if (len == 2) {
            const Fixed first = joinLeft - kFixed1 - ul;
            const Fixed second = ll - ul - first;
            const std::uint8_t a1 = partialTriangleToAlpha(first, lDy);
            const std::uint8_t a2 = static_cast<std::uint8_t>(full - partialTriangleToAlpha(second, lDy));
            blitTwoAlphas(b, row, ul >> 16, a1, a2);
        } else {
            blitAaaTrapezoidRow(b, row, ul, joinLeft, ll, joinLeft, lDy, kMaxS32);
        }
    }
    if (joinLeft < joinRite) blitFullAlpha(b, row, fixedFloor(joinLeft), fixedFloor(joinRite - joinLeft));
    if (lr > joinRite) {
        const std::int32_t len = fixedCeil(lr - joinRite);
        if (len == 1) {
            blitSingleAlpha(b, row, joinRite >> 16, trapezoidToAlpha(ur - joinRite, lr - joinRite));
        } else if (len == 2) {
            const Fixed first = joinRite + kFixed1 - ur;
            const Fixed second = lr - ur - first;
            const std::uint8_t a1 = static_cast<std::uint8_t>(full - partialTriangleToAlpha(first, rDy));
            const std::uint8_t a2 = partialTriangleToAlpha(second, rDy);
            blitTwoAlphas(b, row, joinRite >> 16, a1, a2);
        } else {
            blitAaaTrapezoidRow(b, row, joinRite, ur, joinRite, lr, kMaxS32, rDy);
        }
    }
}

// ------------------------------------------------------------------ walkers

// The sorted edge list between its head (index 0) and tail (index 1)
// sentinels.
constexpr std::size_t kHead = 0;
constexpr std::size_t kTail = 1;

struct EdgeList {
    std::vector<Edge> e;

    explicit EdgeList(std::vector<Edge> edges) {
        sortEdges(edges);
        const std::size_t count = edges.size();
        Edge head;
        head.upperY = kMinS32;
        head.lowerY = kMinS32;
        head.x = kMinS32;
        head.dx = 0;
        head.dy = kMaxS32;
        head.upperX = kMinS32;
        Edge tail;
        tail.upperY = kMaxS32;
        tail.lowerY = kMaxS32;
        tail.x = kMaxS32;
        tail.dx = 0;
        tail.dy = kMaxS32;
        tail.upperX = kMaxS32;
        e.reserve(count + 2);
        e.push_back(head);
        e.push_back(tail);
        e.insert(e.end(), edges.begin(), edges.end());
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t idx = i + 2;
            e[idx].prev = i == 0 ? kHead : idx - 1;
            e[idx].next = i + 1 == count ? kTail : idx + 1;
        }
        e[kHead].next = 2;
        e[kTail].prev = count + 1;
    }

    void remove(std::size_t edge) {
        const std::size_t p = e[edge].prev;
        const std::size_t n = e[edge].next;
        e[p].next = n;
        e[n].prev = p;
    }

    void insertAfter(std::size_t edge, std::size_t after) {
        const std::size_t n = e[after].next;
        e[edge].prev = after;
        e[edge].next = n;
        e[n].prev = edge;
        e[after].next = edge;
    }

    void backwardInsertBasedOnX(std::size_t edge) {
        const Fixed x = e[edge].x;
        std::size_t prev = e[edge].prev;
        while (e[prev].prev != kNone && e[prev].x > x) prev = e[prev].prev;
        if (e[prev].next != edge) {
            remove(edge);
            insertAfter(edge, prev);
        }
    }

    std::size_t backwardInsertStart(std::size_t prev, Fixed x) const {
        while (e[prev].prev != kNone && e[prev].x > x) prev = e[prev].prev;
        return prev;
    }
};

// is_smooth_enough(thisEdge, nextEdge, stop_y).
bool smoothOne(const Edge& thisEdge, const Edge& nextEdge) {
    if (thisEdge.curveCount > 0) {
        return (wabs(thisEdge.qdx) >> 1) >= wabs(thisEdge.qddx) && (wabs(thisEdge.qdy) >> 1) >= wabs(thisEdge.qddy)
            && (wsub(thisEdge.qdy, thisEdge.qddy) >> thisEdge.curveShift) >= kFixed1;
    }
    return wabs(satSub(nextEdge.dx, thisEdge.dx)) <= kFixed1 && wsub(nextEdge.lowerY, nextEdge.upperY) >= kFixed1;
}

// is_smooth_enough(leftE, riteE, currE, stop_y).
bool smoothEnough(const EdgeList& l, std::size_t left, std::size_t rite, std::size_t curr, std::int32_t stopY) {
    const Fixed stop = shl(stopY, 16);
    if (l.e[curr].upperY >= stop) return false;
    const Edge& le = l.e[left];
    const Edge& re = l.e[rite];
    if (wadd(le.lowerY, kFixed1) < re.lowerY) return smoothOne(le, l.e[curr]);
    if (le.lowerY > wadd(re.lowerY, kFixed1)) return smoothOne(re, l.e[curr]);
    std::size_t nextCurr = l.e[curr].next;
    if (l.e[nextCurr].upperY >= stop) return false;
    if (l.e[nextCurr].upperX < l.e[curr].upperX) std::swap(curr, nextCurr);
    return smoothOne(le, l.e[curr]) && smoothOne(re, l.e[nextCurr]);
}

// aaa_walk_convex_edges.
void walkConvex(EdgeList& l, Additive& b, std::int32_t stopY, Fixed leftBound, Fixed riteBound) {
    std::size_t leftE = l.e[kHead].next;
    std::size_t riteE = l.e[leftE].next;
    std::size_t currE = l.e[riteE].next;
    Fixed y = std::max(l.e[leftE].upperY, l.e[riteE].upperY);
    const bool usingMask = b.isMask();

    for (;;) {
        while (l.e[leftE].lowerY <= y) {
            if (!l.e[leftE].update()) {
                if (fixedFloor(l.e[currE].upperY) >= stopY) return;
                leftE = currE;
                currE = l.e[currE].next;
            }
        }
        while (l.e[riteE].lowerY <= y) {
            if (!l.e[riteE].update()) {
                if (fixedFloor(l.e[currE].upperY) >= stopY) return;
                riteE = currE;
                currE = l.e[currE].next;
            }
        }
        if (fixedFloor(y) >= stopY) return;
        l.e[leftE].goY(y);
        l.e[riteE].goY(y);
        {
            const Edge& le = l.e[leftE];
            const Edge& re = l.e[riteE];
            if (le.x > re.x || (le.x == re.x && le.dx > re.dx)) std::swap(leftE, riteE);
        }
        Fixed localBot = std::min(l.e[leftE].lowerY, l.e[riteE].lowerY);
        if (smoothEnough(l, leftE, riteE, currE, stopY)) localBot = fixedCeilToFixed(localBot);
        localBot = std::min(localBot, intToFixed(stopY));

        Fixed left = std::max(leftBound, l.e[leftE].x);
        const Fixed dLeft = l.e[leftE].dx;
        Fixed rite = std::min(riteBound, l.e[riteE].x);
        const Fixed dRite = l.e[riteE].dx;
        if ((dLeft | dRite) == 0) {
            const std::int32_t fullLeft = fixedCeil(left);
            const std::int32_t fullRite = fixedFloor(rite);
            const Fixed partialLeft = intToFixed(fullLeft) - left;
            const Fixed partialRite = rite - intToFixed(fullRite);
            const std::int32_t fullTop = fixedCeil(y);
            const std::int32_t fullBot = fixedFloor(localBot);
            Fixed partialTop = intToFixed(fullTop) - y;
            Fixed partialBot = localBot - intToFixed(fullBot);
            if (fullTop > fullBot) {
                partialTop -= kFixed1 - partialBot;
                partialBot = 0;
            }
            if (fullRite >= fullLeft) {
                if (partialTop > 0) {
                    if (partialLeft > 0) {
                        b.blitAntiH1(fullLeft - 1, fullTop - 1, fixedToAlpha(fixedMul(partialTop, partialLeft)));
                    }
                    b.blitAntiHN(fullLeft, fullTop - 1, fullRite - fullLeft, fixedToAlpha(partialTop));
                    if (partialRite > 0) {
                        b.blitAntiH1(fullRite, fullTop - 1, fixedToAlpha(fixedMul(partialTop, partialRite)));
                    }
                    b.flushIfYChanged(y, y + partialTop);
                }
                if (fullBot > fullTop
                    && (fullRite > fullLeft || fixedToAlpha(partialLeft) > 0 || fixedToAlpha(partialRite) > 0)) {
                    b.realBlitAntiRect(fullLeft - 1, fullTop, fullRite - fullLeft, fullBot - fullTop,
                                       fixedToAlpha(partialLeft), fixedToAlpha(partialRite));
                }
                if (partialBot > 0) {
                    if (partialLeft > 0) {
                        b.blitAntiH1(fullLeft - 1, fullBot, fixedToAlpha(fixedMul(partialBot, partialLeft)));
                    }
                    b.blitAntiHN(fullLeft, fullBot, fullRite - fullLeft, fixedToAlpha(partialBot));
                    if (partialRite > 0) {
                        b.blitAntiH1(fullRite, fullBot, fixedToAlpha(fixedMul(partialBot, partialRite)));
                    }
                }
            } else {
                const Fixed width = rite - left;
                if (width > 0) {
                    if (partialTop > 0) {
                        b.blitAntiHN(fullLeft - 1, fullTop - 1, 1, fixedToAlpha(fixedMul(partialTop, width)));
                        b.flushIfYChanged(y, y + partialTop);
                    }
                    if (fullBot > fullTop) b.realBlitV(fullLeft - 1, fullTop, fullBot - fullTop, fixedToAlpha(width));
                    if (partialBot > 0) {
                        b.blitAntiHN(fullLeft - 1, fullBot, 1, fixedToAlpha(fixedMul(partialBot, width)));
                    }
                }
            }
            y = localBot;
        } else {
            constexpr Fixed kSnapDigit = kFixed1 >> 4;
            constexpr Fixed kSnapHalf = kSnapDigit >> 1;
            constexpr Fixed kSnapMask = -1 ^ (kSnapDigit - 1);
            left += kSnapHalf;
            rite += kSnapHalf;
            std::int32_t count = fixedCeil(localBot) - fixedFloor(y);
            const Fixed lDy = l.e[leftE].dy;
            const Fixed rDy = l.e[riteE].dy;
            const auto row = [usingMask](Fixed rowY, std::uint8_t full) {
                Row r;
                r.y = rowY >> 16;
                r.hasMaskRow = usingMask;
                r.maskRow = rowY >> 16;
                r.full = full;
                r.noReal = false;
                return r;
            };
            if (count > 1) {
                if (static_cast<std::int32_t>(static_cast<std::uint32_t>(y) & 0xFFFF0000u) != y) {
                    count -= 1;
                    const Fixed nextY = fixedCeilToFixed(y + 1);
                    const Fixed dy = nextY - y;
                    const Fixed nextLeft = left + fixedMul(dLeft, dy);
                    const Fixed nextRite = rite + fixedMul(dRite, dy);
                    blitTrapezoidRow(b, row(y, partialAlphaFixed(0xFF, dy)), left & kSnapMask, rite & kSnapMask,
                                     nextLeft & kSnapMask, nextRite & kSnapMask, lDy, rDy);
                    b.flushIfYChanged(y, nextY);
                    left = nextLeft;
                    rite = nextRite;
                    y = nextY;
                }
                while (count > 1) {
                    count -= 1;
                    const Fixed nextY = y + kFixed1;
                    const Fixed nextLeft = left + dLeft;
                    const Fixed nextRite = rite + dRite;
                    blitTrapezoidRow(b, row(y, 0xFF), left & kSnapMask, rite & kSnapMask, nextLeft & kSnapMask,
                                     nextRite & kSnapMask, lDy, rDy);
                    b.flushIfYChanged(y, nextY);
                    left = nextLeft;
                    rite = nextRite;
                    y = nextY;
                }
            }
            const Fixed dy = localBot - y;
            const Fixed nextLeft = std::max(left + fixedMul(dLeft, dy), leftBound + kSnapHalf);
            const Fixed nextRite = std::min(rite + fixedMul(dRite, dy), riteBound + kSnapHalf);
            blitTrapezoidRow(b, row(y, partialAlphaFixed(0xFF, dy)), left & kSnapMask, rite & kSnapMask,
                             nextLeft & kSnapMask, nextRite & kSnapMask, lDy, rDy);
            b.flushIfYChanged(y, localBot);
            left = nextLeft - kSnapHalf;
            rite = nextRite - kSnapHalf;
            y = localBot;
        }
        l.e[leftE].x = left;
        l.e[riteE].x = rite;
        l.e[leftE].y = y;
        l.e[riteE].y = y;
    }
}

void updateNextNextY(Fixed y, Fixed nextY, Fixed& nextNextY) {
    if (y > nextY && y < nextNextY) nextNextY = y;
}

void checkIntersection(const EdgeList& l, std::size_t edge, Fixed nextY, Fixed& nextNextY) {
    const Edge& e = l.e[edge];
    const Edge& prev = l.e[e.prev];
    if (prev.prev != kNone && wadd(prev.x, prev.dx) > wadd(e.x, e.dx)) {
        nextNextY = nextY + (kFixed1 >> kDefaultAccuracy);
    }
}

void checkIntersectionFwd(const EdgeList& l, std::size_t edge, Fixed nextY, Fixed& nextNextY) {
    const Edge& e = l.e[edge];
    const Edge& next = l.e[e.next];
    if (next.next != kNone && wadd(e.x, e.dx) > wadd(next.x, next.dx)) {
        nextNextY = nextY + (kFixed1 >> kDefaultAccuracy);
    }
}

void insertNewEdges(EdgeList& l, std::size_t newEdge, Fixed y, Fixed& nextNextY) {
    if (l.e[newEdge].upperY > y) {
        updateNextNextY(l.e[newEdge].upperY, y, nextNextY);
        return;
    }
    const std::size_t prev = l.e[newEdge].prev;
    if (l.e[prev].x <= l.e[newEdge].x) {
        while (l.e[newEdge].upperY <= y) {
            checkIntersection(l, newEdge, y, nextNextY);
            updateNextNextY(l.e[newEdge].lowerY, y, nextNextY);
            newEdge = l.e[newEdge].next;
        }
        updateNextNextY(l.e[newEdge].upperY, y, nextNextY);
        return;
    }
    std::size_t start = l.backwardInsertStart(prev, l.e[newEdge].x);
    for (;;) {
        const std::size_t next = l.e[newEdge].next;
        bool moved = true;
        for (;;) {
            if (l.e[start].next == newEdge) {
                moved = false;
                break;
            }
            const std::size_t after = l.e[start].next;
            if (l.e[after].x >= l.e[newEdge].x) break;
            start = after;
        }
        if (moved) {
            l.remove(newEdge);
            l.insertAfter(newEdge, start);
        }
        checkIntersection(l, newEdge, y, nextNextY);
        checkIntersectionFwd(l, newEdge, y, nextNextY);
        updateNextNextY(l.e[newEdge].lowerY, y, nextNextY);
        start = newEdge;
        newEdge = next;
        if (l.e[newEdge].upperY > y) break;
    }
    updateNextNextY(l.e[newEdge].upperY, y, nextNextY);
}

bool edgesTooClose(const EdgeList& l, std::size_t prev, std::size_t next, Fixed lowerY) {
    if (prev == kNone || next == kNone) return false;
    const Edge& p = l.e[prev];
    const Edge& n = l.e[next];
    return n.upperY < lowerY && wadd(p.x, kFixed1) >= wsub(n.x, wabs(n.dx));
}

bool edgesTooCloseRite(std::int32_t prevRite, Fixed ul, Fixed ll) {
    return prevRite > fixedFloor(ul) || prevRite > fixedFloor(ll);
}

// aaa_walk_edges for a winding fill, not inverse.
void walkEdges(EdgeList& l, Additive& b, std::int32_t startY, std::int32_t stopY, Fixed leftClip, Fixed rightClip,
               bool skipIntersect) {
    l.e[kHead].x = leftClip;
    l.e[kHead].upperX = leftClip;
    l.e[kTail].x = rightClip;
    l.e[kTail].upperX = rightClip;
    Fixed y = std::max(l.e[l.e[kHead].next].upperY, intToFixed(startY));
    Fixed nextNextY = kMaxS32;
    {
        std::size_t edge = l.e[kHead].next;
        while (l.e[edge].upperY <= y) {
            l.e[edge].goY(y);
            updateNextNextY(l.e[edge].lowerY, y, nextNextY);
            edge = l.e[edge].next;
        }
        updateNextNextY(l.e[edge].upperY, y, nextNextY);
    }

    for (;;) {
        std::int32_t w = 0;
        bool inInterval = false;
        Fixed prevX = l.e[kHead].x;
        Fixed nextY = std::min(nextNextY, fixedCeilToFixed(y + 1));
        std::size_t currE = l.e[kHead].next;
        std::size_t leftE = kHead;
        Fixed left = leftClip;
        Fixed leftDy = 0;
        std::int32_t prevRite = fixedFloor(leftClip);
        nextNextY = kMaxS32;

        std::int32_t yShift = 0;
        if (((nextY - y) & (kFixed1 >> 2)) != 0) {
            yShift = 2;
            nextY = y + (kFixed1 >> 2);
        } else if (((nextY - y) & (kFixed1 >> 1)) != 0) {
            yShift = 1;
        }
        const std::uint8_t fullAlpha = fixedToAlpha(nextY - y);
        const bool hasMaskRow = b.isMask();
        const std::int32_t maskRow = fixedFloor(y);
        const bool noRealBlitter = false;

        while (l.e[currE].upperY <= y) {
            w += l.e[currE].winding;
            const bool prevInInterval = inInterval;
            inInterval = w != 0;
            const bool isLeft = inInterval && !prevInInterval;
            const bool isRite = !inInterval && prevInInterval;

            if (isRite) {
                Fixed rite = l.e[currE].x;
                l.e[currE].goYShift(nextY, yShift);
                const Fixed nextLeft = std::max(leftClip, l.e[leftE].x);
                rite = std::min(rightClip, rite);
                const Fixed nextRite = std::min(rightClip, l.e[currE].x);
                const bool tooClose = fullAlpha == 0xFF
                    && (edgesTooCloseRite(prevRite, left, l.e[leftE].x)
                        || edgesTooClose(l, currE, l.e[currE].next, nextY));
                Row row;
                row.y = y >> 16;
                row.hasMaskRow = hasMaskRow;
                row.maskRow = maskRow;
                row.full = fullAlpha;
                row.noReal = noRealBlitter || tooClose;
                blitTrapezoidRow(b, row, left, rite, nextLeft, nextRite, leftDy, l.e[currE].dy);
                prevRite = fixedCeil(std::max(rite, l.e[currE].x));
            } else {
                if (isLeft) {
                    left = std::max(l.e[currE].x, leftClip);
                    leftDy = l.e[currE].dy;
                    leftE = currE;
                }
                l.e[currE].goYShift(nextY, yShift);
            }

            const std::size_t next = l.e[currE].next;
            while (l.e[currE].lowerY <= nextY) {
                if (l.e[currE].curveCount > 0) {
                    l.e[currE].keepContinuous();
                    if (!l.e[currE].updateQuadratic()) break;
                } else {
                    break;
                }
            }

            if (l.e[currE].lowerY <= nextY) {
                l.remove(currE);
            } else {
                updateNextNextY(l.e[currE].lowerY, nextY, nextNextY);
                const Fixed newX = l.e[currE].x;
                if (newX < prevX) {
                    l.backwardInsertBasedOnX(currE);
                } else {
                    prevX = newX;
                }
                if (!skipIntersect) checkIntersection(l, currE, nextY, nextNextY);
            }
            currE = next;
        }

        if (inInterval) {
            const bool tooClose = fullAlpha == 0xFF && edgesTooClose(l, l.e[leftE].prev, leftE, nextY);
            Row row;
            row.y = y >> 16;
            row.hasMaskRow = hasMaskRow;
            row.maskRow = maskRow;
            row.full = fullAlpha;
            row.noReal = noRealBlitter || tooClose;
            blitTrapezoidRow(b, row, left, rightClip, std::max(leftClip, l.e[leftE].x), rightClip, leftDy, 0);
        }

        y = nextY;
        if (y >= intToFixed(stopY)) break;
        insertNewEdges(l, currE, y, nextNextY);
    }
}

// --------------------------------------------------------------------- entry

// Rounded-out bounds (SkRect::roundOut, saturating).
IRect roundOut(const Rect& r) {
    return {toI32(std::floor(static_cast<double>(r.left))), toI32(std::floor(static_cast<double>(r.top))),
            toI32(std::ceil(static_cast<double>(r.right))), toI32(std::ceil(static_cast<double>(r.bottom)))};
}

// aaa_fill_path with a non-inverse winding fill.
void aaaFillPath(const Path& path, bool convex, const IRect& clip, Additive& b, std::int32_t startY,
                 std::int32_t stopY, bool contained, const IRect& pathIr) {
    std::vector<Edge> edges = EdgeBuilder::build(path, contained ? nullptr : &clip, !convex);
    const std::size_t count = edges.size();
    if (count == 0) return;
    EdgeList list(std::move(edges));
    if (!contained) {
        startY = std::max(startY, clip.top);
        stopY = std::min(stopY, clip.bottom);
    }
    Fixed leftBound = intToFixed(clip.left);
    Fixed riteBound = intToFixed(clip.right);
    if (b.isMask()) {
        leftBound = std::max(leftBound, intToFixed(pathIr.left));
        riteBound = std::min(riteBound, intToFixed(pathIr.right));
    }
    if (convex && count >= 2) {
        walkConvex(list, b, stopY, leftBound, riteBound);
    } else {
        const std::int32_t span = (stopY - startY) * 2;
        const bool skipIntersect = span < 0 || path.pts.size() > static_cast<std::size_t>(span);
        walkEdges(list, b, startY, stopY, leftBound, riteBound, skipIntersect);
    }
}

} // namespace

// Draw::drawPath -> SkScan::AntiFillPath -> SkScan::AAAFillPath.
void fillStroke(Pt p0, Pt p1, float width, const IRect& canvas, Blitter& blitter) {
    const Path path = strokeLine(p0, p1, width);
    Rect bounds;
    if (!path.bounds(bounds)) return;
    // Draw::drawDevPath: SkPathPriv::TooBigForMath.
    const float max = std::numeric_limits<float>::max() * 0.25f;
    if (!(bounds.left >= -max && bounds.top >= -max && bounds.right <= max && bounds.bottom <= max)) return;
    const bool convex = isConvex(path);

    // AntiFillPath: safeRoundOut (rounded out, limited to what the
    // supersampling shift can hold) and the clip intersection.
    const std::int32_t limit = std::numeric_limits<std::int32_t>::max() >> 2;
    IRect ir;
    if (!roundOut(bounds).intersect(IRect{-limit, -limit, limit, limit}, ir)) return;
    if (!ir.intersects(canvas)) return;

    // SkScanClipper: a rectangle clip wraps the blitter only when the path
    // pokes out horizontally.
    const bool contained = canvas.contains(ir);
    RectClipBlitter rectClipped(blitter, canvas);
    Blitter& real =
        !contained && (canvas.left > ir.left || canvas.right < ir.right) ? static_cast<Blitter&>(rectClipped) : blitter;

    // AAAFillPath.
    Additive b;
    if (MaskAdditive::canHandle(ir)) {
        b.maskMode = true;
        b.m.init(ir, canvas);
        aaaFillPath(path, convex, canvas, b, ir.top, ir.bottom, contained, ir);
        real.blitMask(b.m.mask, b.m.clipRect);
    } else {
        b.maskMode = false;
        b.r.init(real, ir, canvas, !convex);
        aaaFillPath(path, convex, canvas, b, ir.top, ir.bottom, contained, ir);
        b.r.flush();
    }
}

} // namespace nm::raster
