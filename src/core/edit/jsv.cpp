#include "core/edit/jsv.h"
#include "core/value/json.h"
#include "core/value/js_number.h"
#include "core/value/js_string.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>

namespace nm {
JsError::JsError(JsString name, JsString message)
    : std::runtime_error(js_to_utf8(message)), name_(std::move(name)), message_(std::move(message)) {}
}

namespace nm::jsv {
namespace {
JsString native_function(const JsString& name) { return u"function " + name + u"() { [native code] }"; }
bool in_names(const JsString& key, std::initializer_list<const char16_t*> names) {
    for (const auto* name : names) if (key == name) return true;
    return false;
}
bool object_proto(const JsString& key) {
    return in_names(key, {u"constructor",u"__defineGetter__",u"__defineSetter__",u"hasOwnProperty",
       u"__lookupGetter__",u"__lookupSetter__",u"isPrototypeOf",u"propertyIsEnumerable",
       u"toString",u"valueOf",u"__proto__",u"toLocaleString"});
}
bool array_index(const JsString& key, size_t& index) {
    if (key.empty() || (key.size()>1 && key[0]==u'0')) return false;
    size_t n=0;
    for (auto c:key) { if (c<u'0'||c>u'9') return false; n=n*10+(c-u'0'); if(n>=4294967295ULL) return false; }
    index=n; return true;
}
Value inherited(const JsString& key, const JsString& ctor) {
    if (key == u"__proto__") return ctor == u"Array" ? Value(Array{}) : Value(Object{});
    if (key == u"constructor") return Value::function(native_function(ctor));
    if (object_proto(key)) return Value::function(native_function(key));
    return {};
}
JsString number_text(double value) { return js_number_to_string(value); }
JsString decimal(size_t n) { return utf8_to_js(std::to_string(n)); }
}
JsError cannot_read(const Value& base, const JsString& key) {
    return JsError(u"TypeError", u"Cannot read properties of " + JsString(base.is_null()?u"null":u"undefined")
                   + u" (reading '" + key + u"')");
}
JsError not_a_function(const JsString& expression) { return JsError(u"TypeError", expression + u" is not a function"); }
JsError stack_overflow() { return JsError(u"RangeError", u"Maximum call stack size exceeded"); }
Value member(const Value& base, const JsString& key) {
    if (base.is_null() || base.is_undefined()) return {};
    if (base.is_object()) {
        if (const Value* v=base.as_object().find(key)) return *v;
        return inherited(key,u"Object");
    }
    if (base.is_array()) {
        const auto& a=base.as_array();
        if(key==u"length") return Value(static_cast<double>(a.size()));
        size_t i=0; if(array_index(key,i)) return i<a.size()?a[i]:Value();
        return inherited(key,u"Array");
    }
    if (base.is_string()) {
        const auto& s=base.as_string();
        if(key==u"length") return Value(static_cast<double>(s.size()));
        size_t i=0; if(array_index(key,i)) return i<s.size()?Value(JsString(1,s[i])):Value();
        return inherited(key,u"String");
    }
    if (base.is_number()) return inherited(key,u"Number");
    if (base.is_bool()) return inherited(key,u"Boolean");
    if (base.is_function()) return inherited(key,u"Function");
    return {};
}
Value get(const Value& base,const JsString& key) {
    if(base.is_null()||base.is_undefined()) throw cannot_read(base,key);
    return member(base,key);
}
Value get_opt(const Value& base,const JsString& key) { return member(base,key); }
Value get_v(const Value& base,const Value& key) { return get(base,to_property_key(key)); }
bool has_property(const Value& base,const JsString& key) {
    if(base.is_object()) return base.as_object().has(key)||object_proto(key);
    if(base.is_array()) { size_t i=0; return key==u"length" || (array_index(key,i)&&i<base.as_array().size()) || object_proto(key); }
    if(base.is_function()) return key==u"prototype"||key==u"name"||key==u"length"||object_proto(key);
    return false;
}
JsString to_property_key(const Value& value) { return to_string(value); }
JsString to_string(const Value& value) {
    if(value.is_undefined()) return u"undefined";
    if(value.is_null()) return u"null";
    if(value.is_bool()) return value.as_bool()?u"true":u"false";
    if(value.is_number()) return number_text(value.as_number());
    if(value.is_string()||value.is_function()) return value.as_string();
    if(value.is_array()) return join(value.as_array(),u",");
    if(value.is_object()) {
        const Value* own= value.as_object().find(u"toString");
        if(!own || own->is_function()) return u"[object Object]";
        const Value* fallback=value.as_object().find(u"valueOf");
        if(fallback && fallback->is_function()) return u"[object Object]";
        throw JsError(u"TypeError",u"Cannot convert object to primitive value");
    }
    return u"undefined";
}
double to_number(const Value& value) {
    if(value.is_undefined()) return std::numeric_limits<double>::quiet_NaN();
    if(value.is_null()) return 0;
    if(value.is_bool()) return value.as_bool()?1:0;
    if(value.is_number()) return value.as_number();
    JsString s=value.is_string()?value.as_string():to_string(value);
    const auto first=s.find_first_not_of(u" \t\n\r\v\f");
    if(first==JsString::npos) return 0;
    const auto last=s.find_last_not_of(u" \t\n\r\v\f");
    std::string ascii=js_to_utf8(s.substr(first,last-first+1));
    double n=0;
    auto parsed=std::from_chars(ascii.data(),ascii.data()+ascii.size(),n);
    if(parsed.ec==std::errc()&&parsed.ptr==ascii.data()+ascii.size()) return n;
    if(ascii=="Infinity"||ascii=="+Infinity") return std::numeric_limits<double>::infinity();
    if(ascii=="-Infinity") return -std::numeric_limits<double>::infinity();
    return std::numeric_limits<double>::quiet_NaN();
}
bool truthy(const Value& value) {
    if(value.is_undefined()||value.is_null()) return false;
    if(value.is_bool()) return value.as_bool();
    if(value.is_number()) return value.as_number()!=0&&!std::isnan(value.as_number());
    if(value.is_string()) return !value.as_string().empty();
    return true;
}
bool strict_equals(const Value& a,const Value& b) {
    if(a.kind()!=b.kind()) return false;
    if(a.is_undefined()||a.is_null()) return true;
    if(a.is_bool()) return a.as_bool()==b.as_bool();
    if(a.is_number()) return a.as_number()==b.as_number();
    if(a.is_string()) return a.as_string()==b.as_string();
    return false;
}
bool same_value_zero(const Value& a,const Value& b) {
    if(a.is_number()&&b.is_number()) return a.as_number()==b.as_number() || (std::isnan(a.as_number())&&std::isnan(b.as_number()));
    return strict_equals(a,b);
}
bool is_finite_number(const Value& value) { return value.is_number()&&std::isfinite(value.as_number()); }
JsString join(const Array& values,const JsString& separator) {
    JsString out;
    for(size_t i=0;i<values.size();++i) { if(i) out+=separator; if(!values[i].is_null()&&!values[i].is_undefined()) out+=to_string(values[i]); }
    return out;
}
bool is_object_like(const Value& value) { return value.is_object()||value.is_array(); }
Entries entries(const Value& value) {
    Entries out;
    if(value.is_object()) for(const auto& key:value.as_object().keys()) out.emplace_back(key,*value.as_object().find(key));
    else if(value.is_array()) for(size_t i=0;i<value.as_array().size();++i) out.emplace_back(decimal(i),value.as_array()[i]);
    else if(value.is_string()) for(size_t i=0;i<value.as_string().size();++i) out.emplace_back(decimal(i),JsString(1,value.as_string()[i]));
    return out;
}
Entries entries_strict(const Value& value) {
    if(value.is_null()||value.is_undefined()) throw JsError(u"TypeError",u"Cannot convert undefined or null to object");
    return entries(value);
}
std::vector<JsString> keys(const Value& value) { std::vector<JsString> out; for(const auto& [k,v]:entries(value)) { (void)v; out.push_back(k); } return out; }
Array values(const Value& value) { Array out; for(const auto& [k,v]:entries(value)) { (void)k; out.push_back(v); } return out; }
void set_plain(Object& object,const JsString& key,Value value) { if(key!=u"__proto__") object.set(key,std::move(value)); }
void spread_into(Object& target,const Value& source) { for(const auto& [k,v]:entries(source)) target.set(k,v); }
Array iterate(const Value& value,const JsString& expression) {
    if(value.is_array()) return value.as_array();
    if(value.is_string()) { Array out; for(char16_t c:value.as_string()) out.emplace_back(JsString(1,c)); return out; }
    throw JsError(u"TypeError",expression+u" is not iterable");
}
Array iterate_anon(const Value& value) {
    if(value.is_array()||value.is_string())return iterate(value,u"object");
    JsString type=u"object";
    if(value.is_number())type=u"number "+js_number_to_string(value.as_number());
    else if(value.is_bool())type=value.as_bool()?u"boolean true":u"boolean false";
    else if(value.is_function())type=u"function";
    throw JsError(u"TypeError",type+u" is not iterable (cannot read property Symbol(Symbol.iterator))");
}
bool in_operator(const Value& key,const Value& object) {
    JsString name=to_property_key(key);
    if(object.is_object()||object.is_array()||object.is_function()) return has_property(object,name);
    throw JsError(u"TypeError",u"Cannot use 'in' operator to search for '"+name+u"' in "+to_string(object));
}
JsString repeat(const JsString& source,double count) {
    double n=std::isnan(count)?0:std::trunc(count);
    if(n<0||std::isinf(n)) throw JsError(u"RangeError",u"Invalid count value: "+number_text(count));
    if(n*source.size()>100000000) throw JsError(u"RangeError",u"Invalid string length");
    JsString out;out.reserve(static_cast<size_t>(n)*source.size());for(size_t i=0;i<static_cast<size_t>(n);++i)out+=source;return out;
}
JsString pad_start(const JsString& source,size_t width,char16_t fill) { if(source.size()>=width)return source;return JsString(width-source.size(),fill)+source; }
JsString number_to_hex(double value) {
    if(std::isnan(value)) return u"NaN"; if(std::isinf(value)) return value>0?u"Infinity":u"-Infinity";
    if(value==0)return u"0";
    bool neg=value<0; if(neg)value=-value;
    JsString out;while(value>=1) { int digit=static_cast<int>(std::fmod(value,16));out.push_back(u"0123456789abcdef"[digit]);value=std::floor(value/16); }
    std::reverse(out.begin(),out.end());return neg?u"-"+out:out;
}
JsString quote_json_string(const JsString& source) { return utf8_to_js(json::stringify(Value(source))); }
JsString quote_json_utf16(const JsString& source) { return quote_json_string(source); }
Value json_parse_string_body(const JsString& raw) {
    try { return json::parse(js_to_utf8(u"\""+raw+u"\"")); } catch(...) { return {}; }
}
}
