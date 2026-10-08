#include "check.h"
#include "core/value/value.h"

int main() {
    using nm::Object; using nm::Value;
    Object o;
    o.set(u"b", Value(1));
    o.set(u"10", Value(2));
    o.set(u"a", Value(3));
    o.set(u"2", Value(4));
    NM_CHECK((o.keys() == std::vector<nm::JsString>{u"2", u"10", u"b", u"a"}));

    Object p;
    p.set(u"01", Value(1));
    p.set(u"-1", Value(2));
    p.set(u"1", Value(3));
    NM_CHECK((p.keys() == std::vector<nm::JsString>{u"1", u"01", u"-1"}));

    Object h;
    h.set(u"x", Value(1));
    h.define_hidden(u"secret", Value(2));
    NM_CHECK(h.has(u"secret"));
    NM_CHECK((h.keys() == std::vector<nm::JsString>{u"x"}));

    Object q;
    q.set(u"a", Value(1));
    q.set(u"b", Value(2));
    q.set(u"a", Value(3));
    NM_CHECK((q.keys() == std::vector<nm::JsString>{u"a", u"b"}));
    return 0;
}
