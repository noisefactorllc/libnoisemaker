#include "check.h"
#include "core/value/json.h"
#include "core/value/parse_double.h"
#include "core/value/value.h"

#include <clocale>
#include <cmath>
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
    NM_CHECK(json::parse("5e-324").as_number() == std::numeric_limits<double>::denorm_min());
    NM_CHECK(json::parse("1e-400").as_number() == 0.0);
    double parsed = 23.0;
    const char* hex = "-0x1";
    const auto hex_result = nm::parse_double(hex, hex + 4, parsed);
    NM_CHECK(hex_result.ec == std::errc{} && hex_result.ptr == hex + 2 && std::signbit(parsed));
    const char* positive = "+1";
    const auto positive_result = nm::parse_double(positive, positive + 2, parsed);
    NM_CHECK(positive_result.ec == std::errc::invalid_argument && positive_result.ptr == positive);
    const std::string old_locale = std::setlocale(LC_NUMERIC, nullptr);
    if (std::setlocale(LC_NUMERIC, "de_DE.UTF-8")) {
        NM_CHECK(json::parse("1.25").as_number() == 1.25);
        std::setlocale(LC_NUMERIC, old_locale.c_str());
    }
    return 0;
}
