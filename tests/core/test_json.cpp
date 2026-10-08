#include "check.h"
#include "core/value/json.h"
#include "core/value/value.h"

#include <clocale>
#include <limits>

int main() {
    using nm::Array; using nm::Object; using nm::Value; namespace json = nm::json;
    NM_CHECK(json::stringify(Value(-0.0)) == "0");
    NM_CHECK(json::stringify(Value(std::numeric_limits<double>::quiet_NaN())) == "null");
    NM_CHECK(json::stringify(Value(std::numeric_limits<double>::infinity())) == "null");
    NM_CHECK(json::stringify(Value(1e21)) == "1e+21");
    NM_CHECK(json::stringify(Value(0.1 + 0.2)) == "0.30000000000000004");
    NM_CHECK(json::stringify(Value(123456789012345680000.0)) == "123456789012345680000");
    NM_CHECK(json::stringify(Value(5e-7)) == "5e-7");
    NM_CHECK(json::stringify(Value(nm::JsString(u"\xD800"))) == "\"\\ud800\"");
    NM_CHECK(json::stringify(Value(nm::JsString(u"\u2028"))) == "\"\xE2\x80\xA8\"");
    NM_CHECK(json::stringify(Value(nm::JsString(u"a\"b\\c\n"))) == "\"a\\\"b\\\\c\\n\"");
    Object o;
    o.set(u"u", Value());
    o.set(u"n", Value::null());
    o.set(u"f", Value::function(u"x => x"));
    o.set(u"a", Value(Array{Value(), Value(1)}));
    NM_CHECK(json::stringify(Value(o)) == "{\"n\":null,\"a\":[null,1]}");
    std::string text = "{\"z\":1,\"1\":2,\"y\":[true,false,null,\"s\"]}";
    NM_CHECK(json::stringify(json::parse(text)) == "{\"1\":2,\"z\":1,\"y\":[true,false,null,\"s\"]}");
    const std::string old_locale = std::setlocale(LC_NUMERIC, nullptr);
    if (std::setlocale(LC_NUMERIC, "de_DE.UTF-8")) {
        NM_CHECK(json::parse("1.25").as_number() == 1.25);
        std::setlocale(LC_NUMERIC, old_locale.c_str());
    }
    return 0;
}
