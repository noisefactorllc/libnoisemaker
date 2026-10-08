#include "core/edit/unparser.h"
#include "core/value/json.h"
#include "core/value/js_number.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace nm {
namespace {
thread_local size_t format_depth=0;
struct DepthGuard {
    DepthGuard(){if(format_depth>=128)throw jsv::stack_overflow();++format_depth;}
    ~DepthGuard(){--format_depth;}
};
Value field(const Value& v, const JsString& key) { return jsv::get_opt(v,key); }
Value own(const Value& v, const JsString& key) {
    if (!v.is_object()) return {};
    const Value* item=v.as_object().find(key);
    return item?*item:Value();
}
bool is(const Value& v,const char16_t* type) { return jsv::strict_equals(field(v,u"type"),Value(type)); }
JsString text(const Value& v) { return jsv::to_string(v); }
Value fallback(const Value& a,const Value& b) { return jsv::truthy(a)?a:b; }
JsString join_text(const std::vector<JsString>& parts,const JsString& separator) {
    JsString out;for(size_t i=0;i<parts.size();++i){if(i)out+=separator;out+=parts[i];}return out;
}
JsString escape_string(const JsString& s) {
    JsString out;out.reserve(s.size());
    for(char16_t ch:s){if(ch==u'\\'||ch==u'"')out.push_back(u'\\');out.push_back(ch);}return out;
}
JsString format_list(const Array& items,const UnparseOptions& options) {
    std::vector<JsString> parts;parts.reserve(items.size());
    for(const auto& item:items) parts.push_back(text(format_value(item,Value::null(),options)));
    return join_text(parts,u", ");
}
bool identifier(const JsString& s) {
    if(s.empty())return false;
    auto letter=[](char16_t c){return (c>=u'a'&&c<=u'z')||(c>=u'A'&&c<=u'Z')||c==u'_';};
    auto tail=[&](char16_t c){return letter(c)||(c>=u'0'&&c<=u'9');};
    if(!letter(s[0]))return false;for(size_t i=1;i<s.size();++i)if(!tail(s[i]))return false;return true;
}
bool enum_path(const JsString& s) {
    size_t start=0,count=0;while(start<s.size()){auto end=s.find(u'.',start);if(!identifier(s.substr(start,end==JsString::npos?JsString::npos:end-start)))return false;++count;if(end==JsString::npos)break;start=end+1;}return count>1;
}
JsString hex_component(const Value& value,bool clamp) {
    double v=jsv::to_number(value)*255.0;
    v=std::floor(v+0.5);
    if(clamp&&!std::isnan(v))v=std::clamp(v,0.0,255.0);
    return jsv::pad_start(jsv::number_to_hex(v),2,u'0');
}
Value hex_color(const Array& items,size_t count,bool clamp) {
    JsString out=u"#";
    for(size_t i=0;i<count;++i)out+=hex_component(i<items.size()?items[i]:Value(),clamp);
    return Value(out);
}
Value ref_type(const Value& value,const Value& spec,const Value& type) {
    if(jsv::strict_equals(type,u"surface")) {
        if(jsv::is_object_like(value)) {
            auto name=field(value,u"name");
            if(jsv::truthy(name))return name.is_string()&&name.as_string()==u"none"?Value(u"none"):Value(u"read("+text(name)+u")");
        }
        if(value.is_string()&&!value.as_string().empty()) {
            if(value.as_string()==u"none"||value.as_string().find(u'(')!=JsString::npos)return value;
            return Value(u"read("+value.as_string()+u")");
        }
        Value def=fallback(field(spec,u"default"),Value(u"inputTex"));
        return text(def)==u"none"?Value(u"none"):Value(u"read("+text(def)+u")");
    }
    bool volume=jsv::strict_equals(type,u"volume");
    if(volume||jsv::strict_equals(type,u"geometry")) {
        if(jsv::is_object_like(value)&&jsv::truthy(field(value,u"name")))return field(value,u"name");
        if(value.is_string()&&!value.as_string().empty())return value;
        return volume?fallback(field(spec,u"default"),Value(u"vol0")):field(spec,u"default");
    }
    return {};
}
Value format_array(const Array& a,const Value& type,const UnparseOptions& options) {
    bool all_numbers=std::all_of(a.begin(),a.end(),[](const Value& v){return v.is_number();});
    bool color=jsv::strict_equals(type,u"color");
    if(all_numbers&&a.size()==2)return Value(u"vec2("+format_list(a,options)+u")");
    if(all_numbers&&a.size()==3)return color?hex_color(a,3,true):Value(u"vec3("+format_list(a,options)+u")");
    if(all_numbers&&a.size()==4)return hex_color(a,4,jsv::strict_equals(type,u"vec4")?false:true);
    if(color&&a.size()>=3)return hex_color(a,3,true);
    Array head;for(size_t i=0;i<std::min<size_t>(3,a.size());++i)head.push_back(a[i]);
    return Value(u"vec3("+format_list(head,options)+u")");
}
Value name_or(const Value& key,const std::vector<JsString>& names,const char16_t* fallback_name) {
    Array values;for(const auto& name:names)values.emplace_back(name);
    return fallback(jsv::get_v(Value(values),key),Value(fallback_name));
}
Value format_oscillator(const Value& v) {
    static const char16_t* names[]={u"sine",u"tri",u"saw",u"sawInv",u"square",u"noise1d",u"noise2d"};
    Value ast=field(v,u"_ast");if(is(ast,u"Oscillator"))return format_let_expr(ast);
    Value kind=jsv::get(v,u"oscType");
    JsString name=text(name_or(kind,std::vector<JsString>(std::begin(names),std::end(names)),u"sine"));
    std::vector<JsString> parts{u"type: oscKind."+name};
    for(const auto& [key,def]:std::initializer_list<std::pair<const char16_t*,double>>{{u"min",0},{u"max",1},{u"speed",1},{u"offset",0},{u"seed",1}}){
        Value val=jsv::get(v,key);if(!jsv::strict_equals(val,Value(def))&&(JsString(key)!=u"seed"||jsv::strict_equals(kind,Value(5))))parts.push_back(JsString(key)+u": "+text(val));
    }
    return Value(u"osc("+join_text(parts,u", ")+u")");
}
Value format_midi(const Value& v) {
    static const char16_t* names[]={u"noteChange",u"gateNote",u"gateVelocity",u"triggerNote",u"velocity",u"cc",u"cc14",u"nrpn",u"pitchBend",u"pressure",u"polyPressure"};
    Value channel=jsv::get(v,u"channel"),zone=jsv::get(v,u"zone");
    if((jsv::truthy(channel)&&jsv::is_object_like(channel))||(jsv::truthy(zone)&&jsv::is_object_like(zone)))return format_let_expr(v);
    Value ast=field(v,u"_ast");if(is(ast,u"Midi"))return format_let_expr(ast);
    std::vector<JsString> parts;
    if(!zone.is_undefined())parts.push_back(u"zone: midiZone."+JsString(jsv::strict_equals(zone,0)?u"lower":u"upper"));
    else parts.push_back(u"channel: "+text(channel));
    Value members=jsv::get(v,u"members");if(!members.is_undefined())parts.push_back(u"members: "+text(members));
    Value mode=jsv::get(v,u"mode");
    JsString mode_name=text(name_or(mode,std::vector<JsString>(std::begin(names),std::end(names)),u"velocity"));
    if(mode_name!=u"velocity")parts.push_back(u"mode: midiMode."+mode_name);
    for(const char16_t* key:{u"cc",u"nrpn"}){Value x=jsv::get(v,key);if(!x.is_undefined())parts.push_back(JsString(key)+u": "+text(x));}
    for(const auto& [key,def]:std::initializer_list<std::pair<const char16_t*,double>>{{u"min",0},{u"max",1},{u"sensitivity",1}}){Value x=jsv::get(v,key);if(!jsv::strict_equals(x,Value(def)))parts.push_back(JsString(key)+u": "+text(x));}
    for(const char16_t* key:{u"name",u"id"}){Value x=jsv::get(v,key);if(x.is_string()&&!x.as_string().empty())parts.push_back(JsString(key)+u": "+jsv::quote_json_string(x.as_string()));}
    return Value(u"midi("+join_text(parts,u", ")+u")");
}
Value format_audio(const Value& v) {
    static const char16_t* names[]={u"low",u"mid",u"high",u"vol",u"raw"};
    Value band=jsv::get(v,u"band");if(jsv::truthy(band)&&jsv::is_object_like(band))return format_let_expr(v);
    Value ast=field(v,u"_ast");if(is(ast,u"Audio"))return format_let_expr(ast);
    std::vector<JsString> parts{u"band: audioBand."+text(name_or(band,std::vector<JsString>(std::begin(names),std::end(names)),u"low"))};
    for(const auto& [key,def]:std::initializer_list<std::pair<const char16_t*,double>>{{u"min",0},{u"max",1}}){Value x=jsv::get(v,key);if(!jsv::strict_equals(x,Value(def)))parts.push_back(JsString(key)+u": "+text(x));}
    Value channel=jsv::get(v,u"channel");if(channel.is_number()&&std::floor(channel.as_number())==channel.as_number()&&channel.as_number()>=1)parts.push_back(u"channel: "+text(channel));
    for(const char16_t* key:{u"name",u"id"}){Value x=jsv::get(v,key);if(x.is_string()&&!x.as_string().empty())parts.push_back(JsString(key)+u": "+jsv::quote_json_string(x.as_string()));}
    return Value(u"audio("+join_text(parts,u", ")+u")");
}
Value format_object(const Value& value,const Value& spec,const UnparseOptions& options) {
    Value ast=field(value,u"_ast");
    if(is(value,u"String")){
        Value raw=jsv::get(value,u"value");
        if(raw.is_null()||raw.is_undefined())throw jsv::cannot_read(raw,u"replace");
        if(!raw.is_string())throw jsv::not_a_function(u"value.value.replace");
        return Value(u"\""+escape_string(raw.as_string())+u"\"");
    }
    if(is(value,u"Oscillator")||jsv::strict_equals(field(value,u"oscillator"),Value(true)))return format_oscillator(value);
    if(is(value,u"Midi"))return format_midi(value);
    if(is(value,u"Audio"))return format_audio(value);
    if(is(ast,u"Oscillator"))return format_oscillator(value);
    if(is(ast,u"Midi"))return format_midi(value);
    if(is(ast,u"Audio"))return format_audio(value);
    if(is(value,u"Read")){Value surface=field(value,u"surface");return Value(u"read("+text(fallback(field(surface,u"name"),surface))+u")");}
    if(is(value,u"Read3D")){Value t=field(value,u"tex3d"),g=field(value,u"geo");JsString out=u"read3d("+text(fallback(field(t,u"name"),t));if(jsv::truthy(g))out+=u", "+text(fallback(field(g,u"name"),g));return Value(out+u")");}
    for(const char16_t* kind:{u"OutputRef",u"SourceRef",u"VolRef",u"GeoRef"})if(is(value,kind))return field(value,u"name");
    if(is(value,u"Member")){
        Value path=field(value,u"path");
        if(path.is_null()||path.is_undefined())throw jsv::cannot_read(path,u"join");
        if(!path.is_array())throw jsv::not_a_function(u"value.path.join");
        return Value(jsv::join(path.as_array(),u"."));
    }
    if(is(value,u"Number"))return format_value(field(value,u"value"),spec,options);
    if(is(value,u"Boolean"))return Value(jsv::truthy(field(value,u"value"))?u"true":u"false");
    Value kind=field(value,u"kind");if(jsv::strict_equals(kind,u"output")||jsv::strict_equals(kind,u"feedback")||jsv::strict_equals(kind,u"source")){
        Value name=field(value,u"name");return jsv::strict_equals(field(spec,u"type"),u"surface")?Value(u"read("+text(name)+u")"):name;
    }
    Value length=field(value,u"length");if(length.is_number()){
        double n=length.as_number();
        n=(std::isnan(n)||n<=0)?0:std::min(std::trunc(n),9007199254740991.0);
        if(n>4294967295.0)throw JsError(u"RangeError",u"Invalid array length");
        if(n>=2&&n<=4){
            Array a;for(size_t i=0;i<static_cast<size_t>(n);++i)a.push_back(field(value,utf8_to_js(std::to_string(i))));
            if(std::all_of(a.begin(),a.end(),[](const Value& x){return x.is_number();}))return format_array(a,field(spec,u"type"),options);
        }
    }
    return Value(jsv::to_string(value));
}
void merge_enums(Object& target,const Value& source) {
    if(!source.is_object())return;
    for(const auto& key:source.as_object().keys()){
        if(key==u"__proto__"||key==u"constructor"||key==u"prototype")continue;
        Value value=*source.as_object().find(key);
        Value existing=target.has(key)?*target.find(key):Value();
        if(value.is_object()&&existing.is_object()&&!value.as_object().has(u"type")){
            Object nested=existing.as_object();merge_enums(nested,value);target.set(key,Value(nested));
        }else target.set(key,value);
    }
}
}
UnparseOptions UnparseOptions::from_value(const Value& source,const EffectRegistry* reg) {
    UnparseOptions o;o.registry=reg;o.enums=field(source,u"enums");o.omit_search_directive=field(source,u"omitSearchDirective");o.multiline_kwargs=field(source,u"multilineKwargs");o.indent=field(source,u"indent");o.specs=field(source,u"specs");o.schema_specs=field(source,u"schemaSpecs");o.lookup_spec=field(source,u"getEffectDef");o.custom_formatter_spec=field(source,u"customFormatter");
    Value legacy=field(source,u"$legacyFormatter");
    if(!legacy.is_undefined()){
        o.enums=Value();o.omit_search_directive=Value();o.multiline_kwargs=Value();o.indent=Value();o.specs=Value();o.schema_specs=Value();o.lookup_spec=Value();
        o.custom_formatter_spec=legacy;
    }
    if(reg&&o.enums.is_object()){
        auto name=field(o.enums,u"$enums");if(name.is_string()){
            if(name.as_string()==u"host"){
                Object merged=reg->enums().std().raw().as_object();
                merge_enums(merged,reg->enums().project().raw());o.enums=Value(merged);
            }else o.enums=reg->enums().std().raw();
        }
    }
    if(reg&&field(o.lookup_spec,u"kind").is_string()){
        Value spec=o.lookup_spec;
        o.get_effect_def=[reg,spec](const Value& name,const Value& ns)->Value{
            auto kind=field(spec,u"kind");
            if(kind.as_string()==u"constant")return field(spec,u"def");
            if(kind.as_string()==u"map")return field(field(spec,u"defs"),text(name));
            if(kind.as_string()==u"ops") {
                Value found=jsv::member(field(reg->dumpSummaryValue(),u"ops"),jsv::to_property_key(name));
                return jsv::truthy(found)?found:Value::null();
            }
            if(!name.is_string())return kind.as_string()==u"registrySimple"?Value():Value::null();
            JsString key=name.as_string();Value direct=reg->getEffectValue(key);if(direct.is_object())return direct;
            auto dot=key.find(u'.');if(dot!=JsString::npos){direct=reg->getEffectValue(key.substr(0,dot)+u"/"+key.substr(dot+1));if(direct.is_object())return direct;}
            if(jsv::truthy(ns)){JsString prefix=text(ns);direct=reg->getEffectValue(prefix+u"/"+key);if(direct.is_object())return direct;direct=reg->getEffectValue(prefix+u"."+key);if(direct.is_object())return direct;}
            return kind.as_string()==u"registrySimple"?Value():Value::null();
        };
    }
    if(reg&&jsv::strict_equals(field(o.custom_formatter_spec,u"kind"),Value(u"formatValue"))){
        Object config;config.set(u"enums",field(o.custom_formatter_spec,u"enums"));
        UnparseOptions nested=UnparseOptions::from_value(Value(config),reg);
        o.custom_formatter=[nested](const Value& value,const Value& spec)->Value{
            return format_value(value,spec,nested);
        };
    }
    return o;
}
JsString format_lossless_number(double number) {
    std::string s=js_to_utf8(js_number_to_string(number));auto e=s.find_first_of("eE");if(e==std::string::npos)return utf8_to_js(s);
    std::string coefficient=s.substr(0,e);int exponent=std::stoi(s.substr(e+1));bool negative=!coefficient.empty()&&coefficient[0]=='-';if(negative)coefficient.erase(0,1);
    auto dot=coefficient.find('.');int index=static_cast<int>(dot==std::string::npos?coefficient.size():dot)+exponent;if(dot!=std::string::npos)coefficient.erase(dot,1);
    std::string out=negative?"-":"";if(index<=0)out+="0."+std::string(-index,'0')+coefficient;else if(index>=static_cast<int>(coefficient.size()))out+=coefficient+std::string(index-coefficient.size(),'0');else out+=coefficient.substr(0,index)+"."+coefficient.substr(index);
    return utf8_to_js(out);
}
Value format_value(const Value& value,const Value& spec,const UnparseOptions& options,const Value& source_form) {
    DepthGuard guard;
    if(jsv::strict_equals(field(value,u"kind"),u"temp")&&options.format_temp)return options.format_temp(field(value,u"index"));
    if(options.custom_formatter){Value custom=options.custom_formatter(value,spec);if(!custom.is_null()&&!custom.is_undefined())return custom;}
    if(value.is_null()||value.is_undefined())return Value(u"null");
    Value type=field(spec,u"type");
    if(jsv::strict_equals(type,u"vec4")&&jsv::strict_equals(field(field(spec,u"ui"),u"format"),u"vector")&&value.is_array()&&value.as_array().size()==4&&std::all_of(value.as_array().begin(),value.as_array().end(),jsv::is_finite_number)){
        std::vector<JsString> parts;for(const auto& v:value.as_array())parts.push_back(format_lossless_number(v.as_number()));return Value(u"["+join_text(parts,u", ")+u"]");
    }
    if(jsv::strict_equals(source_form,u"array")&&value.is_array())return Value(u"["+format_list(value.as_array(),options)+u"]");
    if(jsv::is_object_like(value)&&jsv::truthy(field(value,u"_varRef")))return field(value,u"_varRef");
    Value ast=field(value,u"_ast");if(is(ast,u"Ident"))return field(ast,u"name");
    if(value.is_bool())return Value(value.as_bool()?u"true":u"false");
    Value choices=field(spec,u"choices");if(jsv::truthy(choices)&&value.is_number())for(const auto& [name,item]:jsv::entries(choices))if(!name.empty()&&name.back()!=u':'&&jsv::strict_equals(item,value)){JsString n=name;if(n.size()>=4&&n.substr(n.size()-4)==u"Enum")n.resize(n.size()-4);return Value(n);}
    Value enum_spec=field(spec,u"enum");
    if(jsv::truthy(enum_spec)&&value.is_number()){
        if(!enum_spec.is_string())throw jsv::not_a_function(u"enumPath.split");
        Value node=options.enums;JsString path=enum_spec.as_string();size_t pos=0;
        while(pos<=path.size()){auto end=path.find(u'.',pos);node=field(node,path.substr(pos,end==JsString::npos?JsString::npos:end-pos));if(end==JsString::npos)break;pos=end+1;}
        for(const auto& [name,item]:jsv::entries(node)){
            Value candidate=jsv::has_property(item,u"value")?field(item,u"value"):item;
            if(jsv::strict_equals(candidate,value))return Value(name);
        }
    }
    if(jsv::truthy(enum_spec)&&value.is_string()) {JsString prefix=text(enum_spec)+u".";if(value.as_string().rfind(prefix,0)==0)return Value(value.as_string().substr(prefix.size()));}
    if(jsv::strict_equals(type,u"surface")||jsv::strict_equals(type,u"volume")||jsv::strict_equals(type,u"geometry"))return ref_type(value,spec,type);
    if(jsv::strict_equals(type,u"member")||jsv::strict_equals(type,u"palette"))return value;
    if(jsv::is_object_like(value)) {if(is(value,u"Oscillator")||jsv::strict_equals(field(value,u"oscillator"),Value(true)))return format_oscillator(value);if(is(value,u"Midi"))return format_midi(value);if(is(value,u"Audio"))return format_audio(value);}
    if(value.is_number())return Value(format_lossless_number(value.as_number()));
    if(value.is_string()){
        const auto& s=value.as_string();if(!s.empty()&&s[0]==u'#')return value;
        bool quoted=jsv::strict_equals(type,u"string")||(!identifier(s)&&!enum_path(s));
        if(quoted){if(s.find(u'\n')!=JsString::npos)return Value(u"\"\"\""+s+u"\"\"\"");return Value(u"\""+escape_string(s)+u"\"");}
        return value;
    }
    if(value.is_array())return format_array(value.as_array(),type,options);
    if(value.is_object())return format_object(value,spec,options);
    return Value(jsv::to_string(value));
}
JsString unparse_call(const Value& call,const UnparseOptions& options) {
    Value name=jsv::get(call,u"name");
    std::vector<Value> parts;
    Value specs=jsv::truthy(options.specs)?options.specs:Value(Object{});
    bool multiline=!jsv::strict_equals(options.multiline_kwargs,Value(false));
    double base=jsv::is_finite_number(options.indent)?options.indent.as_number():0;
    JsString parent_indent=jsv::repeat(u" ",std::max(0.0,base));
    JsString child_indent=jsv::repeat(u" ",std::max(0.0,base+2));
    Value kwargs=jsv::get(call,u"kwargs");
    size_t kwarg_count=jsv::truthy(kwargs)?jsv::keys(kwargs).size():0;
    if(kwarg_count)for(const auto& [key,value]:jsv::entries(kwargs)){
        if(key==u"_skip"&&jsv::strict_equals(value,Value(false)))continue;
        Value spec=fallback(jsv::member(specs,key),Value::null());
        Value default_spec=jsv::truthy(spec)?spec:fallback(field(options.schema_specs,key),Value::null());
        Value source_form=field(field(call,u"argSources"),key);
        if(jsv::truthy(default_spec)){
            Value def=field(default_spec,u"default");
            if(!def.is_undefined()){
                Value formatted=format_value(value,spec,options,source_form);
                Value formatted_default=format_value(def,spec,options);
                bool explicit_none=jsv::strict_equals(field(spec,u"type"),u"surface")&&jsv::strict_equals(formatted,u"none")&&!jsv::strict_equals(formatted_default,u"none");
                if(jsv::strict_equals(formatted,formatted_default)&&!explicit_none)continue;
            }
        }
        parts.emplace_back(key+u": "+text(format_value(value,spec,options,source_form)));
    }
    Value args=jsv::get(call,u"args");
    if(jsv::truthy(args)&&jsv::to_number(field(args,u"length"))>0)for(const auto& arg:jsv::iterate(args,u"call.args"))parts.push_back(format_value(arg,Value::null(),options));
    bool has_kwargs=kwarg_count>0&&!parts.empty();
    JsString op=text(name);
    if(multiline&&has_kwargs&&parts.size()>2){
        std::vector<JsString> lines;for(const auto& part:parts)lines.push_back(child_indent+text(part));
        return op+u"(\n"+join_text(lines,u",\n")+u"\n"+parent_indent+u")";
    }
    return op+u"("+jsv::join(parts,u", ")+u")";
}
JsString unparse_chain(const Value& chain,const UnparseOptions& options) {
    if(chain.is_null()||chain.is_undefined())throw jsv::cannot_read(chain,u"map");
    if(!chain.is_array())throw jsv::not_a_function(u"chain.map");
    std::vector<JsString> parts;for(size_t i=0;i<chain.as_array().size();++i){UnparseOptions o=options;o.indent=Value(i?2:0);parts.push_back(unparse_call(chain.as_array()[i],o));}
    return join_text(parts,u"\n  .");
}
namespace {
Value numeric_node(const Value& node) {return is(node,u"Number")?field(node,u"value"):Value();}
JsString decoded_literal(const Value& raw) {
    JsString s=text(raw);Value parsed=jsv::json_parse_string_body(s);
    if(parsed.is_string())return parsed.as_string();
    JsString out;
    auto append_escape=[&](const JsString& next){
        if(next.size()!=1){out.push_back(u'\\');out+=next;return;}
        char16_t ch=next[0];
        switch(ch){
        case u'\'':out.push_back(u'\'');break;case u'"':out.push_back(u'"');break;case u'\\':out.push_back(u'\\');break;
        case u'n':out.push_back(u'\n');break;case u'r':out.push_back(u'\r');break;case u't':out.push_back(u'\t');break;
        case u'b':out.push_back(u'\b');break;case u'f':out.push_back(u'\f');break;case u'v':out.push_back(u'\v');break;
        case u'0':out.push_back(u'\0');break;default:out.push_back(u'\\');out.push_back(ch);break;
        }
    };
    if(!raw.is_string()){
        double length=jsv::to_number(jsv::member(raw,u"length"));
        for(size_t i=0;static_cast<double>(i)<length&&i<10000;++i){
            Value item=jsv::get_v(raw,Value(static_cast<double>(i)));
            if(jsv::strict_equals(item,Value(u"\\"))&&static_cast<double>(i+1)<length){
                append_escape(text(jsv::get_v(raw,Value(static_cast<double>(++i)))));
            }else out+=text(item);
        }
        return out;
    }
    for(size_t i=0;i<s.size();++i){
        if(s[i]!=u'\\'||i+1>=s.size()){out.push_back(s[i]);continue;}
        char16_t next=s[++i];
        switch(next){
        case u'\'':out.push_back(u'\'');break;case u'"':out.push_back(u'"');break;case u'\\':out.push_back(u'\\');break;
        case u'n':out.push_back(u'\n');break;case u'r':out.push_back(u'\r');break;case u't':out.push_back(u'\t');break;
        case u'b':out.push_back(u'\b');break;case u'f':out.push_back(u'\f');break;case u'v':out.push_back(u'\v');break;
        case u'0':out.push_back(u'\0');break;default:out.push_back(u'\\');out.push_back(next);break;
        }
    }
    return out;
}
JsString quoted_decoded(const Value& raw) {return jsv::quote_json_utf16(decoded_literal(raw));}
Value last_segment(const Value& path) {
    double index=jsv::to_number(jsv::get(path,u"length"))-1;
    return jsv::get_v(path,Value(index));
}
void push_if_non_default(std::vector<JsString>& parts,const char16_t* name,const Value& node,double def,const UnparseOptions& options,bool has_default=true) {
    if(!jsv::truthy(node))return;
    if(has_default&&jsv::strict_equals(numeric_node(node),Value(def)))return;
    parts.push_back(JsString(name)+u": "+text(format_let_expr(node,options)));
}
Value format_let_oscillator(const Value& expr,const UnparseOptions& options) {
    Value kind=field(expr,u"oscType");JsString type=u"oscKind.sine";
    if(is(kind,u"Member")&&jsv::truthy(field(kind,u"path")))type=u"oscKind."+text(last_segment(field(kind,u"path")));
    else if(is(kind,u"Ident"))type=text(field(kind,u"name"));
    else if(is(kind,u"Number")){Value n=field(kind,u"value");static const char16_t* names[]={u"sine",u"tri",u"saw",u"sawInv",u"square",u"noise1d"};if(n.is_number()&&n.as_number()>=0&&n.as_number()<=5&&std::floor(n.as_number())==n.as_number())type=u"oscKind."+JsString(names[static_cast<int>(n.as_number())]);}
    std::vector<JsString> parts{u"type: "+type};
    for(const auto& [key,def]:std::initializer_list<std::pair<const char16_t*,double>>{{u"min",0},{u"max",1},{u"speed",1},{u"offset",0},{u"seed",1}})push_if_non_default(parts,key,field(expr,key),def,options);
    return Value(u"osc("+join_text(parts,u", ")+u")");
}
Value format_let_midi(const Value& expr,const UnparseOptions& options) {
    std::vector<JsString> parts;Value channel=field(expr,u"channel");if(jsv::truthy(channel))parts.push_back(u"channel: "+text(format_let_expr(channel,options)));
    Value mode=field(expr,u"mode");JsString mode_text;
    if(is(mode,u"Member")&&jsv::truthy(field(mode,u"path"))){JsString n=text(last_segment(field(mode,u"path")));if(n!=u"velocity")mode_text=u"midiMode."+n;}
    else if(is(mode,u"Ident"))mode_text=text(field(mode,u"name"));
    else if(is(mode,u"Number")&&!jsv::strict_equals(field(mode,u"value"),4))mode_text=text(field(mode,u"value"));
    if(!mode_text.empty())parts.push_back(u"mode: "+mode_text);
    for(const auto& [key,def]:std::initializer_list<std::pair<const char16_t*,double>>{{u"min",0},{u"max",1},{u"sensitivity",1}})push_if_non_default(parts,key,field(expr,key),def,options);
    for(const char16_t* key:{u"cc",u"nrpn",u"zone",u"members"})push_if_non_default(parts,key,field(expr,key),0,options,false);
    for(const char16_t* key:{u"name",u"id"}){Value n=field(expr,key);if(is(n,u"String"))parts.push_back(JsString(key)+u": "+quoted_decoded(field(n,u"value")));}
    return Value(u"midi("+join_text(parts,u", ")+u")");
}
Value format_let_audio(const Value& expr,const UnparseOptions& options) {
    Value band=field(expr,u"band");std::vector<JsString> parts{u"band: "+(jsv::truthy(band)?(is(band,u"String")?quoted_decoded(field(band,u"value")):text(format_let_expr(band,options))):JsString(u"audioBand.low"))};
    for(const auto& [key,def]:std::initializer_list<std::pair<const char16_t*,double>>{{u"min",0},{u"max",1},{u"channel",0}}){
        Value node=field(expr,key);if(!jsv::truthy(node))continue;
        if(JsString(key)!=u"channel"&&jsv::strict_equals(numeric_node(node),Value(def)))continue;
        parts.push_back(JsString(key)+u": "+(is(node,u"String")?quoted_decoded(field(node,u"value")):text(format_let_expr(node,options))));
    }
    for(const char16_t* key:{u"name",u"id"}){Value n=field(expr,key);if(is(n,u"String"))parts.push_back(JsString(key)+u": "+quoted_decoded(field(n,u"value")));}
    return Value(u"audio("+join_text(parts,u", ")+u")");
}
}
Value format_let_expr(const Value& expr,const UnparseOptions& options) {
    DepthGuard guard;
    if(!jsv::truthy(expr))return Value(u"null");
    Value var=field(expr,u"_varRef");if(jsv::truthy(var))return var;
    Value type=field(expr,u"type");if(!type.is_string())return format_value(expr,Value::null(),options);
    const auto& t=type.as_string();
    if(t==u"Number")return Value(text(field(expr,u"value")));
    if(t==u"String"){
        Value raw=field(expr,u"value");
        if(raw.is_undefined()||raw.is_function())return Value();
        return Value(utf8_to_js(json::stringify(raw)));
    }
    if(t==u"Boolean")return Value(jsv::truthy(field(expr,u"value"))?u"true":u"false");
    if(t==u"Ident")return field(expr,u"name");
    if(t==u"Member"){
        Value path=field(expr,u"path");
        if(path.is_null()||path.is_undefined())throw jsv::cannot_read(path,u"join");
        if(!path.is_array())throw jsv::not_a_function(u"expr.path.join");
        return Value(jsv::join(path.as_array(),u"."));
    }
    if(t==u"Oscillator")return format_let_oscillator(expr,options);
    if(t==u"Midi")return format_let_midi(expr,options);
    if(t==u"Audio")return format_let_audio(expr,options);
    if(t==u"Call")return Value(unparse_call(expr,options));
    if(t==u"Chain")return Value(unparse_chain(field(expr,u"chain"),options));
    if(t==u"Func")return Value(u"() => "+text(format_let_expr(field(expr,u"body"),options)));
    return format_value(expr,Value::null(),options);
}
namespace {
struct ChainElement { JsString code; Value comments; bool sub_begin=false,sub_end=false; };
struct StepEntry { Value temp, step, override; };
const StepEntry* lookup_step(const std::vector<StepEntry>& entries,const Value& key) {
    for(const auto& entry:entries)if(jsv::same_value_zero(entry.temp,key))return &entry;
    return nullptr;
}
void collect_dependencies(const std::vector<StepEntry>& entries,const Value& index,Array& collected) {
    Array pending{index};
    auto has=[&](const Value& key){return std::any_of(collected.begin(),collected.end(),[&](const Value& v){return jsv::same_value_zero(v,key);});};
    while(!pending.empty()){
        Value key=pending.back();pending.pop_back();
        if(has(key))continue;
        const StepEntry* entry=lookup_step(entries,key);if(!entry)continue;
        collected.push_back(key);
        Array next;
        Value from=field(entry->step,u"from");if(!from.is_null()&&!from.is_undefined())next.push_back(from);
        Object merged;jsv::spread_into(merged,field(entry->step,u"args"));jsv::spread_into(merged,entry->override);
        for(const auto& arg_key:merged.keys()){
            Value val=*merged.find(arg_key);
            if(jsv::strict_equals(field(val,u"kind"),Value(u"temp")))next.push_back(field(val,u"index"));
        }
        for(auto it=next.rbegin();it!=next.rend();++it)pending.push_back(*it);
    }
}
JsString join_chain(const std::vector<ChainElement>& chain) {
    std::vector<JsString> parts;bool in_sub=false;
    for(size_t i=0;i<chain.size();++i){const auto& e=chain[i];
        if(e.comments.is_array())for(const auto& comment:e.comments.as_array())parts.push_back((i==0?JsString():in_sub?JsString(u"    "):JsString(u"  "))+text(comment));
        if(e.sub_begin){parts.push_back((i==0?JsString():JsString(u"  ."))+e.code);in_sub=true;continue;}
        if(e.sub_end){parts.push_back(u"  "+e.code);in_sub=false;continue;}
        parts.push_back((i==0?JsString():in_sub?JsString(u"    ."):JsString(u"  ."))+e.code);
    }
    return join_text(parts,u"\n");
}
Value name_or_self(const Value& v) {return fallback(field(v,u"name"),v);}
Value override_at(const Value& overrides,size_t index) {return jsv::get(overrides,utf8_to_js(std::to_string(index)));}
Value specs_for(const OpSpec* op) {
    Object result;if(!op)return Value(result);
    for(const auto& arg:op->args){Object spec;spec.set(u"name",arg.name);spec.set(u"type",arg.type);if(arg.hasDefault())spec.set(u"default",arg.defaultValue.raw());if(arg.hasEnumPath())spec.set(u"enum",arg.enumPath.raw());if(arg.hasChoices())spec.set(u"choices",arg.choicesValue.raw());result.set(arg.name,Value(spec));}
    return Value(result);
}
}
JsString unparse(const Value& compiled,const Value& override_input,const UnparseOptions& options_input,const EffectRegistry& registry) {
    DepthGuard guard;
    Value overrides=override_input.is_undefined()?Value(Object{}):override_input;
    UnparseOptions options=options_input;if(!jsv::truthy(options.enums))options.enums=registry.enums().std().raw();
    std::vector<JsString> lines;
    Value search=fallback(jsv::get(compiled,u"searchNamespaces"),Value(Array{}));
    if(jsv::to_number(jsv::member(search,u"length"))>0&&!jsv::truthy(options.omit_search_directive)){
        if(!search.is_array())throw jsv::not_a_function(u"searchNamespaces.join");
        lines.push_back(u"search "+jsv::join(search.as_array(),u", "));lines.emplace_back();
    }
    Value vars=jsv::get(compiled,u"vars");
    if(jsv::truthy(vars)&&jsv::to_number(jsv::member(vars,u"length"))>0){
        for(const auto& v:jsv::iterate(vars,u"compiled.vars")){
            Value comments=field(v,u"leadingComments");if(comments.is_array())for(const auto& c:comments.as_array())lines.push_back(c.is_null()||c.is_undefined()?JsString():text(c));
            lines.push_back(u"let "+text(jsv::get(v,u"name"))+u" = "+text(format_let_expr(jsv::get(v,u"expr"),options)));
        }
        lines.emplace_back();
    }
    Value plans=fallback(jsv::get(compiled,u"plans"),Value(Array{}));
    double plan_count=jsv::to_number(jsv::member(plans,u"length"));
    size_t global_index=0;
    for(size_t pi=0;static_cast<double>(pi)<plan_count;++pi){
        Value plan=jsv::get_v(plans,Value(static_cast<double>(pi)));Value chain=jsv::get(plan,u"chain");
        if(!jsv::truthy(chain)||jsv::strict_equals(jsv::member(chain,u"length"),Value(0)))continue;
        if(!chain.is_array())throw jsv::not_a_function(u"plan.chain.map");
        const auto& steps=chain.as_array();
        std::vector<StepEntry> steps_by_temp;
        for(size_t i=0;i<steps.size();++i){
            Value temp=field(steps[i],u"temp");Value step_override=fallback(override_at(overrides,global_index+i),Value(Object{}));
            bool found=false;
            for(auto& entry:steps_by_temp)if(jsv::same_value_zero(entry.temp,temp)){entry.step=steps[i];entry.override=step_override;found=true;break;}
            if(!found)steps_by_temp.push_back({temp,steps[i],step_override});
        }
        Array inline_temps;
        for(const auto& entry:steps_by_temp){
            Array args=jsv::values(fallback(field(entry.step,u"args"),Value(Object{})));
            Array override_args=jsv::values(entry.override);args.insert(args.end(),override_args.begin(),override_args.end());
            for(const auto& val:args)if(jsv::strict_equals(field(val,u"kind"),Value(u"temp")))
                collect_dependencies(steps_by_temp,field(val,u"index"),inline_temps);
        }
        std::vector<std::pair<Value,Value>> inline_code;
        UnparseOptions plan_options=options;
        plan_options.format_temp=[&,steps_by_temp,search,options](const Value& index)->Value{
            for(const auto& [key,code]:inline_code)if(jsv::same_value_zero(key,index))return code;
            Array dependencies;collect_dependencies(steps_by_temp,index,dependencies);
            Array nested_chain;Object nested_overrides;
            for(const auto& entry:steps_by_temp){
                bool included=std::any_of(dependencies.begin(),dependencies.end(),[&](const Value& v){return jsv::same_value_zero(v,entry.temp);});
                if(!included)continue;
                nested_overrides.set(utf8_to_js(std::to_string(nested_chain.size())),entry.override);
                nested_chain.push_back(entry.step);
            }
            Object nested_plan;nested_plan.set(u"chain",Value(std::move(nested_chain)));
            Object nested;nested.set(u"searchNamespaces",search);nested.set(u"plans",Value(Array{Value(std::move(nested_plan))}));
            UnparseOptions nested_options=options;nested_options.multiline_kwargs=Value(false);nested_options.omit_search_directive=Value(true);
            Value code(unparse(Value(std::move(nested)),Value(std::move(nested_overrides)),nested_options,registry));
            inline_code.emplace_back(index,code);return code;
        };
        Value comments=field(plan,u"leadingComments");if(comments.is_array())for(const auto& c:comments.as_array())lines.push_back(c.is_null()||c.is_undefined()?JsString():text(c));
        std::vector<std::vector<ChainElement>> chains;std::vector<ChainElement> current;bool in_sub=false;
        for(const auto& step:steps){
            bool is_inline=std::any_of(inline_temps.begin(),inline_temps.end(),[&](const Value& v){return jsv::same_value_zero(v,field(step,u"temp"));});
            if(is_inline){++global_index;continue;}
            Value op=jsv::get(step,u"op"),args=jsv::get(step,u"args");JsString op_name=text(op);
            bool builtin=jsv::truthy(jsv::get(step,u"builtin"));
            Value step_override=override_at(overrides,global_index);
            Value leading=field(step,u"leadingComments");
            auto add=[&](JsString code,bool begin=false,bool end=false){current.push_back({std::move(code),leading,begin,end});};
            if(builtin&&(op_name==u"_read"||op_name==u"_read3d")){
                if(!current.empty()){chains.push_back(std::move(current));current.clear();}
                bool skip=jsv::strict_equals(field(step_override,u"_skip"),Value(true))||
                    (field(step_override,u"_skip").is_undefined()&&jsv::strict_equals(field(args,u"_skip"),Value(true)));
                if(op_name==u"_read"){
                    JsString tex=text(name_or_self(field(args,u"tex")));
                    add(skip?u"read(surface: "+tex+u", _skip: true)":u"read("+tex+u")");
                }else{
                    JsString tex=text(name_or_self(field(args,u"tex3d"))),geo=text(name_or_self(field(args,u"geo")));
                    add(skip?u"read3d(tex3d: "+tex+u", geo: "+geo+u", _skip: true)":u"read3d("+tex+u", "+geo+u")");
                }
                ++global_index;continue;
            }
            if(builtin&&op_name==u"_write"){add(u"write("+text(name_or_self(field(args,u"tex")))+u")");++global_index;continue;}
            if(builtin&&op_name==u"_write3d"){add(u"write3d("+text(name_or_self(field(args,u"tex3d")))+u", "+text(name_or_self(field(args,u"geo")))+u")");++global_index;continue;}
            if(builtin&&op_name==u"_subchain_begin"){
                std::vector<JsString> fields;Value n=field(args,u"name"),id=field(args,u"id");if(jsv::truthy(n))fields.push_back(u"name: \""+text(n)+u"\"");if(jsv::truthy(id))fields.push_back(u"id: \""+text(id)+u"\"");add(u"subchain("+join_text(fields,u", ")+u") {",true);in_sub=true;++global_index;continue;
            }
            if(builtin&&op_name==u"_subchain_end"){current.push_back({u"}",Value(),false,true});in_sub=false;++global_index;continue;}
            Value effect_def=Value::null();
            if(options.get_effect_def){Value ns=field(step,u"namespace");effect_def=options.get_effect_def(op,fallback(field(ns,u"namespace"),fallback(field(ns,u"resolved"),Value::null())));}
            Value namespace_value=field(step,u"namespace");Value call_ns=fallback(field(namespace_value,u"call"),namespace_value);
            bool from_override=jsv::strict_equals(field(call_ns,u"fromOverride"),Value(true));
            Value from_namespace=from_override?fallback(field(call_ns,u"resolved"),fallback(field(call_ns,u"name"),field(namespace_value,u"resolved"))):Value::null();
            JsString call_name=op_name;
            for(const auto& ns:jsv::iterate(search,u"searchNamespaces")){
                if(!op.is_string()){
                    if(op.is_null()||op.is_undefined())throw jsv::cannot_read(op,u"startsWith");
                    throw jsv::not_a_function(u"callName.startsWith");
                }
                JsString prefix=text(ns)+u".";if(call_name.rfind(prefix,0)==0){call_name.erase(0,prefix.size());break;}
            }
            if(from_override&&jsv::truthy(from_namespace)){JsString prefix=text(from_namespace)+u".";if(call_name.rfind(prefix,0)==0)call_name.erase(0,prefix.size());}
            Object kwargs;
            if(jsv::truthy(args))for(const auto& [key,val]:jsv::entries(args)){
                if(key==u"from"||key==u"temp")continue;
                if(key==u"_skip"&&!jsv::strict_equals(val,Value(true)))continue;
                Value kind=field(val,u"kind");
                jsv::set_plain(kwargs,key,jsv::is_object_like(val)&&jsv::truthy(kind)&&!jsv::strict_equals(kind,u"temp")?field(val,u"name"):val);
            }
            Value specs=fallback(field(effect_def,u"globals"),Value(Object{}));
            Value schema=jsv::truthy(effect_def)?Value(Object{}):specs_for(registry.getOp(JsText(op_name)));
            if(jsv::truthy(step_override))for(const auto& [key,val]:jsv::entries(step_override)){
                if(!key.empty()&&key[0]==u'_')jsv::set_plain(kwargs,key,val);
                else if(!jsv::truthy(effect_def)||!field(specs,key).is_undefined())jsv::set_plain(kwargs,key,val);
            }
            if(kwargs.has(u"volumeSize")&&jsv::strict_equals(field(field(field(specs,u"volumeSize"),u"ui"),u"control"),Value(false)))kwargs.erase(u"volumeSize");
            Object call;call.set(u"name",Value(call_name));call.set(u"kwargs",Value(kwargs));call.set(u"args",Value(Array{}));
            Value arg_sources=field(step,u"argSources");if(jsv::truthy(arg_sources))call.set(u"argSources",arg_sources);
            UnparseOptions call_options=plan_options;call_options.specs=specs;call_options.schema_specs=schema;call_options.indent=Value(current.empty()?0:in_sub?4:2);
            JsString code=unparse_call(Value(call),call_options);
            if(from_override&&jsv::truthy(from_namespace))code=u"from("+text(from_namespace)+u", "+code+u")";
            add(code);++global_index;
        }
        if(!current.empty())chains.push_back(std::move(current));
        std::vector<JsString> chunks;for(const auto& chunk:chains)chunks.push_back(join_chain(chunk));JsString line=join_text(chunks,u"\n\n");
        Value last=steps.back();bool ends_write=jsv::truthy(field(last,u"builtin"))&&jsv::strict_equals(field(last,u"op"),u"_write");
        bool ends_write3d=jsv::truthy(field(last,u"builtin"))&&jsv::strict_equals(field(last,u"op"),u"_write3d");
        Value write=jsv::get(plan,u"write");if(jsv::truthy(write)&&!ends_write)line+=u"\n  .write("+text(write.is_string()?write:jsv::get(write,u"name"))+u")";
        Value write3d=jsv::get(plan,u"write3d");if(jsv::truthy(write3d)&&!ends_write3d)line+=u"\n  .write3d("+text(name_or_self(field(write3d,u"tex3d")))+u", "+text(name_or_self(field(write3d,u"geo")))+u")";
        lines.push_back(line);if(static_cast<double>(pi+1)<plan_count)lines.emplace_back();
    }
    Value render=jsv::get(compiled,u"render");if(jsv::truthy(render)){lines.emplace_back();lines.push_back(u"render("+text(render)+u")");}
    Value trailing=jsv::get(compiled,u"trailingComments");
    if(jsv::truthy(trailing)&&jsv::to_number(jsv::member(trailing,u"length"))>0)
        for(const auto& c:jsv::iterate(trailing,u"compiled.trailingComments"))lines.push_back(c.is_null()||c.is_undefined()?JsString():text(c));
    return join_text(lines,u"\n");
}
JsString apply_parameter_updates(const JsString& dsl,const Value& compiled,const Value& updates,const EffectRegistry& registry) {
    if(!jsv::truthy(compiled)||!jsv::truthy(field(compiled,u"plans")))return dsl;
    auto is_line=[](char16_t c){return c==u'\n'||c==u'\r'||c==u'\u2028'||c==u'\u2029';};
    auto is_space=[&](char16_t c){return c==u' '||c==u'\t'||c==u'\v'||c==u'\f'||is_line(c)||c==u'\u00a0'||c==u'\ufeff'||c==u'\u1680'||
                                      (c>=u'\u2000'&&c<=u'\u200a')||c==u'\u202f'||c==u'\u205f'||c==u'\u3000';};
    Value source=compiled.is_object()?Value(compiled.as_object()):compiled;
    for(size_t start=0;start<dsl.size();){
        bool match=dsl.size()>=start+6&&dsl.substr(start,6)==u"search";
        if(match){
            size_t p=start+6,ws=p;while(p<dsl.size()&&is_space(dsl[p]))++p;
            if(p>ws&&p<dsl.size()&&!is_space(dsl[p])){
                size_t end=p+1;while(end<dsl.size()&&!is_line(dsl[end]))++end;
                JsString list=dsl.substr(p,end-p);Array namespaces;size_t begin=0;
                for(size_t i=0;i<list.size();){
                    size_t q=i;while(q<list.size()&&is_space(list[q]))++q;
                    if(q<list.size()&&list[q]==u','){
                        namespaces.emplace_back(list.substr(begin,i-begin));
                        i=q+1;while(i<list.size()&&is_space(list[i]))++i;begin=i;
                    }else ++i;
                }
                namespaces.emplace_back(list.substr(begin));
                if(source.is_object())source.as_object().set(u"searchNamespaces",Value(std::move(namespaces)));
                break;
            }
        }
        while(start<dsl.size()&&!is_line(dsl[start]))++start;
        if(start<dsl.size())++start;
    }
    return unparse(source,updates,UnparseOptions{},registry);
}
} // namespace nm
