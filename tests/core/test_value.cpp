#include "check.h"
#include "core/edit/jsv.h"
#include "core/value/json.h"
#include "core/value/value.h"

#include <cmath>

int main() {
    using libnoisemaker::Object; using libnoisemaker::Value;
    Object o;
    o.set(u"b", Value(1));
    o.set(u"10", Value(2));
    o.set(u"a", Value(3));
    o.set(u"2", Value(4));
    NM_CHECK((o.keys() == std::vector<libnoisemaker::JsString>{u"2", u"10", u"b", u"a"}));

    Object p;
    p.set(u"01", Value(1));
    p.set(u"-1", Value(2));
    p.set(u"1", Value(3));
    NM_CHECK((p.keys() == std::vector<libnoisemaker::JsString>{u"1", u"01", u"-1"}));

    Object h;
    h.set(u"x", Value(1));
    h.define_hidden(u"secret", Value(2));
    NM_CHECK(h.has(u"secret"));
    NM_CHECK((h.keys() == std::vector<libnoisemaker::JsString>{u"x"}));

    Object q;
    q.set(u"a", Value(1));
    q.set(u"b", Value(2));
    q.set(u"a", Value(3));
    NM_CHECK((q.keys() == std::vector<libnoisemaker::JsString>{u"a", u"b"}));
    // A Float32Array keeps float precision, copies by value and reads like
    // the JavaScript typed array.
    const Value typed = Value::float32_array({0.1f, -2.0f});
    NM_CHECK(typed.is_float32_array() && typed.kind() == Value::Kind::Float32Array);
    NM_CHECK(typed.as_float32_array() == (std::vector<float>{0.1f, -2.0f}));
    const Value copy = typed;
    NM_CHECK(&copy.as_float32_array() != &typed.as_float32_array() && copy.as_float32_array() == typed.as_float32_array());
    NM_CHECK(libnoisemaker::json::stringify(typed) == "{\"0\":0.10000000149011612,\"1\":-2}");
    NM_CHECK(libnoisemaker::json::stringify(Value::float32_array({})) == "{}");
    NM_CHECK(libnoisemaker::jsv::to_string(typed) == u"0.10000000149011612,-2");
    NM_CHECK(libnoisemaker::jsv::member(typed, u"length").as_number() == 2);
    NM_CHECK(libnoisemaker::jsv::member(typed, u"1").as_number() == -2);
    NM_CHECK(libnoisemaker::jsv::member(typed, u"2").is_undefined());
    NM_CHECK(libnoisemaker::jsv::has_property(typed, u"0") && !libnoisemaker::jsv::has_property(typed, u"2"));
    NM_CHECK(libnoisemaker::jsv::entries(typed).size() == 2 && libnoisemaker::jsv::truthy(Value::float32_array({})));
    NM_CHECK(std::isnan(libnoisemaker::jsv::to_number(typed)) && libnoisemaker::jsv::to_number(Value::float32_array({3.5f})) == 3.5);
    const Value pair = Value::float32_array({1.5f, 2.0f});
    NM_CHECK(libnoisemaker::jsv::in_operator(Value(u"0"), pair) && !libnoisemaker::jsv::in_operator(Value(u"2"), pair));
    NM_CHECK(libnoisemaker::jsv::in_operator(Value(u"length"), pair));
    const auto iterated = libnoisemaker::jsv::iterate(pair, u"pair");
    NM_CHECK(iterated.size() == 2 && iterated[0].as_number() == 1.5 && iterated[1].as_number() == 2);
    NM_CHECK(libnoisemaker::jsv::iterate_anon(pair).size() == 2);
    NM_CHECK(libnoisemaker::jsv::member(pair, u"byteLength").as_number() == 8);
    NM_CHECK(libnoisemaker::jsv::member(pair, u"byteOffset").as_number() == 0);
    NM_CHECK(libnoisemaker::jsv::member(pair, u"BYTES_PER_ELEMENT").as_number() == 4);
    NM_CHECK(libnoisemaker::jsv::member(pair, u"buffer").is_object());
    NM_CHECK(libnoisemaker::jsv::to_string(libnoisemaker::jsv::member(pair, u"map")) == u"function map() { [native code] }");
    NM_CHECK(libnoisemaker::jsv::to_string(libnoisemaker::jsv::member(pair, u"constructor")) ==
             u"function Float32Array() { [native code] }");
    for (const char16_t* name : {u"buffer", u"byteLength", u"subarray", u"toString", u"valueOf", u"hasOwnProperty"}) {
        NM_CHECK(libnoisemaker::jsv::in_operator(Value(name), pair));
    }
    NM_CHECK(!libnoisemaker::jsv::in_operator(Value(u"push"), pair));
    return 0;
}
