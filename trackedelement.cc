/*
    This file is part of Kismet

    Kismet is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Kismet is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Kismet; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include "config.h"

#include <vector>
#include <stdexcept>

#include "util.h"

#include "trackedelement.h"
#include "globalregistry.h"
#include "entrytracker.h"

#include "endian_magic.h"

#include "alphanum.hpp"

device_key::device_key() {
    spkey = 0;
    dkey = 0;
    error = true;
}

device_key::device_key(const device_key& k) {
    spkey = k.spkey;
    dkey = k.dkey;
    error = k.error;
}

device_key::device_key(uint32_t in_pkey, uint64_t in_dkey) {
    spkey = in_pkey & 0xFFFFFFFF;
    dkey = in_dkey;
    error = false;
}

device_key::device_key(uint32_t in_pkey, mac_addr in_device) {
    spkey = in_pkey & 0xFFFFFFFF;
    dkey = in_device.longmac;
    error = false;
}

device_key::device_key(std::string in_keystr) {
    unsigned long long int k1, k2;

    if (sscanf(in_keystr.c_str(), "%llx_%llx", &k1, &k2) != 2) {
        error = true;
        spkey = 0;
        dkey = 0;
        return;
    }

    // Convert from big endian exported format
    spkey = (uint64_t) kis_ntoh64(k1);
    dkey = (uint64_t) kis_ntoh64(k2);
    error = false;
}

std::string device_key::as_string() const {
    std::stringstream ss;
    ss << *this;
    return ss.str();
}

uint32_t device_key::gen_pkey(std::string phy) {
    return adler32_checksum(phy.c_str(), phy.length());
}

uint64_t device_key::gen_spkey(uuid s_uuid, std::string phy) {
    uint64_t uuid32 = adler32_checksum((const char *) s_uuid.uuid_block, 16);
    uint64_t phy32 = gen_pkey(phy);

    return (uuid32 << 32) | phy32;
}

bool operator <(const device_key& x, const device_key& y) {
    if (x.spkey == y.spkey)
        return x.dkey < y.dkey;

    return x.spkey < y.spkey;
}

bool operator ==(const device_key& x, const device_key& y) {
    return (x.spkey == y.spkey && x.dkey == y.dkey);
}

std::ostream& operator<<(std::ostream& os, const device_key& k) {
    std::ios::fmtflags fflags;

    fflags = os.flags();
    os << std::uppercase << std::setfill('0') << std::setw(2) <<
        std::hex << kis_hton64(k.spkey) << "_" << kis_hton64(k.dkey);
    os.flags(fflags);
    return os;
}

std::istream& operator>>(std::istream& is, device_key& k) {
    std::string sline;
    std::getline(is, sline);

    k = device_key(sline);

    if (k.error)
        is.setstate(std::ios::failbit);

    return is;
}

// New

void tracker_element_string::coercive_set(const std::string& in_str) {
    value = in_str;
}

void tracker_element_string::coercive_set(double in_num) {
    value = fmt::format("{}", in_num);
}

void tracker_element_string::coercive_set(const shared_tracker_element& e) {
    switch (e->get_type()) {
        case tracker_type::tracker_int8:
        case tracker_type::tracker_uint8:
        case tracker_type::tracker_int16:
        case tracker_type::tracker_uint16:
        case tracker_type::tracker_int32:
        case tracker_type::tracker_uint32:
        case tracker_type::tracker_int64:
        case tracker_type::tracker_uint64:
        case tracker_type::tracker_float:
        case tracker_type::tracker_double:
            coercive_set(std::static_pointer_cast<tracker_element_core_scalar>(e)->get());
            break;
        case tracker_type::tracker_string:
            coercive_set(std::static_pointer_cast<tracker_element_string>(e)->get());
            break;
        case tracker_type::tracker_uuid:
            coercive_set(std::static_pointer_cast<tracker_element_uuid>(e)->get().uuid_to_string());
            break;
        case tracker_type::tracker_mac_addr:
            coercive_set(std::static_pointer_cast<tracker_element_mac_addr>(e)->get().mac_to_string());
            break;
        default:
            throw std::runtime_error(fmt::format("Could not coerce {} to {}",
                        e->get_type_as_string(), get_type_as_string()));
    }
}

bool tracker_element_string::less_than(const tracker_element_string& rhs) const {
    return doj::alphanum_comp(value, rhs.value) < 0;
}

void tracker_element_uuid::coercive_set(const std::string& in_str) {
    uuid u(in_str);

    if (u.error)
        throw std::runtime_error("Could not coerce string to UUID");

    value = u;
}

void tracker_element_uuid::coercive_set(double in_num) {
    throw std::runtime_error("Cannot coerce UUID from number");
}

void tracker_element_uuid::coercive_set(const shared_tracker_element& e) {
    switch (e->get_type()) {
        case tracker_type::tracker_uuid:
            coercive_set(std::static_pointer_cast<tracker_element_uuid>(e)->get().uuid_to_string());
            break;
        default:
            throw std::runtime_error(fmt::format("Could not coerce {} to {}",
                        e->get_type_as_string(), get_type_as_string()));
    }
}

void tracker_element_mac_addr::coercive_set(const std::string& in_str) {
    mac_addr m(in_str);

    if (m.error)
        throw std::runtime_error("Could not coerce string to macaddr");

    value = m;
}

void tracker_element_mac_addr::coercive_set(double in_num) {
    throw std::runtime_error("Cannot coerce macaddr from number");
}

void tracker_element_mac_addr::coercive_set(const shared_tracker_element& e) {
    switch (e->get_type()) {
        case tracker_type::tracker_mac_addr:
            coercive_set(std::static_pointer_cast<tracker_element_mac_addr>(e)->get().mac_to_string());
            break;
        default:
            throw std::runtime_error(fmt::format("Could not coerce {} to {}",
                        e->get_type_as_string(), get_type_as_string()));
    }
}

std::string tracker_element::type_to_string(tracker_type t) {
    switch (t) {
        case tracker_type::tracker_string:
            return "string";
        case tracker_type::tracker_int8:
            return "int8_t";
        case tracker_type::tracker_uint8:
            return "uint8_t";
        case tracker_type::tracker_int16:
            return "int16_t";
        case tracker_type::tracker_uint16:
            return "uint16_t";
        case tracker_type::tracker_int32:
            return "int32_t";
        case tracker_type::tracker_uint32:
            return "uint32_t";
        case tracker_type::tracker_int64:
            return "int64_t";
        case tracker_type::tracker_uint64:
            return "uint64_t";
        case tracker_type::tracker_float:
            return "float";
        case tracker_type::tracker_double:
            return "double";
        case tracker_type::tracker_mac_addr:
            return "mac_addr";
        case tracker_type::tracker_vector:
            return "vector[x]";
        case tracker_type::tracker_map:
            return "map[field, x]";
        case tracker_type::tracker_int_map:
            return "map[int, x]";
        case tracker_type::tracker_uuid:
            return "uuid";
        case tracker_type::tracker_key:
            return "devicekey";
        case tracker_type::tracker_mac_map:
            return "map[macaddr, x]";
        case tracker_type::tracker_string_map:
            return "map[string, x]";
        case tracker_type::tracker_double_map:
            return "map[double, x]";
        case tracker_type::tracker_key_map:
            return "map[key, x]";
        case tracker_type::tracker_byte_array:
            return "bytearray";
        case tracker_type::tracker_vector_double:
            return "vector[double]";
        case tracker_type::tracker_double_map_double:
            return "map[double,double]";
        case tracker_type::tracker_vector_string:
            return "vector[string]";
        case tracker_type::tracker_hashkey_map:
            return "vector[size_t]";
        case tracker_type::tracker_alias:
            return "alias";
    }

    return "unknown";
}

std::string tracker_element::type_to_typestring(tracker_type t) {
    switch (t) {
        case tracker_type::tracker_string:
            return "tracker_string";
        case tracker_type::tracker_int8:
            return "tracker_int8";
        case tracker_type::tracker_uint8:
            return "tracker_uint8";
        case tracker_type::tracker_int16:
            return "tracker_int16";
        case tracker_type::tracker_uint16:
            return "tracker_uint16";
        case tracker_type::tracker_int32:
            return "tracker_int32";
        case tracker_type::tracker_uint32:
            return "tracker_uint32";
        case tracker_type::tracker_int64:
            return "tracker_int64";
        case tracker_type::tracker_uint64:
            return "tracker_uint64";
        case tracker_type::tracker_float:
            return "tracker_float";
        case tracker_type::tracker_double:
            return "tracker_double";
        case tracker_type::tracker_mac_addr:
            return "tracker_mac_addr";
        case tracker_type::tracker_vector:
            return "tracker_vector";
        case tracker_type::tracker_map:
            return "tracker_map";
        case tracker_type::tracker_int_map:
            return "tracker_int_map";
        case tracker_type::tracker_uuid:
            return "tracker_uuid";
        case tracker_type::tracker_key:
            return "tracker_key";
        case tracker_type::tracker_mac_map:
            return "tracker_mac_map";
        case tracker_type::tracker_string_map:
            return "tracker_string_map";
        case tracker_type::tracker_double_map:
            return "tracker_double_map";
        case tracker_type::tracker_byte_array:
            return "tracker_byte_array";
        case tracker_type::tracker_key_map:
            return "tracker_key_map";
        case tracker_type::tracker_vector_double:
            return "tracker_vector_double";
        case tracker_type::tracker_double_map_double:
            return "tracker_double_map_double";
        case tracker_type::tracker_vector_string:
            return "tracker_vector_string";
        case tracker_type::tracker_hashkey_map:
            return "tracker_hashkey_map";
        case tracker_type::tracker_alias:
            return "tracker_alias";
    }

    return "TrackerUnknown";
}

tracker_type tracker_element::typestring_to_type(const std::string& s) {
    if (s == "tracker_string")
        return tracker_type::tracker_string;
    if (s == "tracker_int8")
        return tracker_type::tracker_int8;
    if (s == "tracker_uint8")
        return tracker_type::tracker_uint8;
    if (s == "tracker_int16")
        return tracker_type::tracker_int16;
    if (s == "tracker_uint16")
        return tracker_type::tracker_uint16;
    if (s == "tracker_int32")
        return tracker_type::tracker_int32;
    if (s == "tracker_uint32")
        return tracker_type::tracker_uint32;
    if (s == "tracker_int64")
        return tracker_type::tracker_int64;
    if (s == "tracker_uint64")
        return tracker_type::tracker_uint64;
    if (s == "tracker_float")
        return tracker_type::tracker_float;
    if (s == "tracker_double")
        return tracker_type::tracker_double;
    if (s == "tracker_mac_addr")
        return tracker_type::tracker_mac_addr;
    if (s == "tracker_vector")
        return tracker_type::tracker_vector;
    if (s == "tracker_map")
        return tracker_type::tracker_map;
    if (s == "tracker_int_map")
        return tracker_type::tracker_int_map;
    if (s == "tracker_uuid")
        return tracker_type::tracker_uuid;
    if (s == "tracker_key")
        return tracker_type::tracker_key;
    if (s == "tracker_mac_map")
        return tracker_type::tracker_mac_map;
    if (s == "tracker_string_map")
        return tracker_type::tracker_string_map;
    if (s == "tracker_double_map")
        return tracker_type::tracker_double_map;
    if (s == "tracker_byte_array")
        return tracker_type::tracker_byte_array;
    if (s == "tracker_key_map")
        return tracker_type::tracker_key_map;
    if (s == "tracker_vector_double")
        return tracker_type::tracker_vector_double;
    if (s == "tracker_double_map_double")
        return tracker_type::tracker_double_map_double;
    if (s == "tracker_vector_string")
        return tracker_type::tracker_vector_string;
    if (s == "tracker_hashkey_map")
        return tracker_type::tracker_hashkey_map;
    if (s == "tracker_alias")
        return tracker_type::tracker_alias;

    throw std::runtime_error("Unable to interpret tracker type " + s);
}

template<> std::string get_tracker_value(const shared_tracker_element& e) {
#if TE_TYPE_SAFETY == 1
    e->enforce_type(tracker_type::tracker_string);
#endif
    return std::static_pointer_cast<tracker_element_string>(e)->get();
}

template<> uint8_t get_tracker_value(const shared_tracker_element& e) {
#if TE_TYPE_SAFETY == 1
    e->enforce_type(tracker_type::tracker_uint8);
#endif
    return std::static_pointer_cast<tracker_element_uint8>(e)->get();
}

template<> int8_t get_tracker_value(const shared_tracker_element& e) {
#if TE_TYPE_SAFETY == 1
    e->enforce_type(tracker_type::tracker_int8);
#endif
    return std::static_pointer_cast<tracker_element_int8>(e)->get();
}

template<> uint16_t get_tracker_value(const shared_tracker_element& e) {
#if TE_TYPE_SAFETY == 1
    e->enforce_type(tracker_type::tracker_uint16);
#endif
    return std::static_pointer_cast<tracker_element_uint16>(e)->get();
}

template<> int16_t get_tracker_value(const shared_tracker_element& e) {
#if TE_TYPE_SAFETY == 1
    e->enforce_type(tracker_type::tracker_int16);
#endif
    return std::static_pointer_cast<tracker_element_int16>(e)->get();
}

template<> uint32_t get_tracker_value(const shared_tracker_element& e) {
#if TE_TYPE_SAFETY == 1
    e->enforce_type(tracker_type::tracker_uint32);
#endif
    return std::static_pointer_cast<tracker_element_uint32>(e)->get();
}

template<> int32_t get_tracker_value(const shared_tracker_element& e) {
#if TE_TYPE_SAFETY == 1
    e->enforce_type(tracker_type::tracker_int32);
#endif
    return std::static_pointer_cast<tracker_element_int32>(e)->get();
}

template<> uint64_t get_tracker_value(const shared_tracker_element& e) {
#if TE_TYPE_SAFETY == 1
    e->enforce_type(tracker_type::tracker_uint64);
#endif
    return std::static_pointer_cast<tracker_element_uint64>(e)->get();
}

template<> int64_t get_tracker_value(const shared_tracker_element& e) {
#if TE_TYPE_SAFETY == 1
    e->enforce_type(tracker_type::tracker_int64);
#endif
    return std::static_pointer_cast<tracker_element_int64>(e)->get();
}

template<> float get_tracker_value(const shared_tracker_element& e) {
#if TE_TYPE_SAFETY == 1
    e->enforce_type(tracker_type::tracker_float);
#endif
    return std::static_pointer_cast<tracker_element_float>(e)->get();
}

template<> double get_tracker_value(const shared_tracker_element& e) {
#if TE_TYPE_SAFETY == 1
    e->enforce_type(tracker_type::tracker_double);
#endif
    return std::static_pointer_cast<tracker_element_double>(e)->get();
}

template<> mac_addr get_tracker_value(const shared_tracker_element& e) {
#if TE_TYPE_SAFETY == 1
    e->enforce_type(tracker_type::tracker_mac_addr);
#endif
    return std::static_pointer_cast<tracker_element_mac_addr>(e)->get();
}

template<> uuid get_tracker_value(const shared_tracker_element& e) {
#if TE_TYPE_SAFETY == 1
    e->enforce_type(tracker_type::tracker_uuid);
#endif
    return std::static_pointer_cast<tracker_element_uuid>(e)->get();
}

template<> device_key get_tracker_value(const shared_tracker_element& e) {
#if TE_TYPE_SAFETY == 1
    e->enforce_type(tracker_type::tracker_key);
#endif
    return std::static_pointer_cast<tracker_element_device_key>(e)->get();
}

template<> void SetTrackerValue(const shared_tracker_element& e, const std::string& v) {
#if TE_TYPE_SAFETY == 1
    e->enforce_type(tracker_type::tracker_string, tracker_type::tracker_byte_array);
#endif
    std::static_pointer_cast<tracker_element_string>(e)->set(v);
}

template<> void SetTrackerValue(const shared_tracker_element& e, const uint8_t& v) {
#if TE_TYPE_SAFETY == 1
    e->enforce_type(tracker_type::tracker_uint8);
#endif
    std::static_pointer_cast<tracker_element_uint8>(e)->set(v);
}

template<> void SetTrackerValue(const shared_tracker_element& e, const int8_t& v) {
#if TE_TYPE_SAFETY == 1
    e->enforce_type(tracker_type::tracker_int8);
#endif
    std::static_pointer_cast<tracker_element_int8>(e)->set(v);
}

template<> void SetTrackerValue(const shared_tracker_element& e, const uint16_t& v) {
#if TE_TYPE_SAFETY == 1
    e->enforce_type(tracker_type::tracker_uint16);
#endif
    std::static_pointer_cast<tracker_element_uint16>(e)->set(v);
}

template<> void SetTrackerValue(const shared_tracker_element& e, const int16_t& v) {
#if TE_TYPE_SAFETY == 1
    e->enforce_type(tracker_type::tracker_int16);
#endif
    std::static_pointer_cast<tracker_element_int16>(e)->set(v);
}

template<> void SetTrackerValue(const shared_tracker_element& e, const uint32_t& v) {
#if TE_TYPE_SAFETY == 1
    e->enforce_type(tracker_type::tracker_uint32);
#endif
    std::static_pointer_cast<tracker_element_uint32>(e)->set(v);
}

template<> void SetTrackerValue(const shared_tracker_element& e, const int32_t& v) {
#if TE_TYPE_SAFETY == 1
    e->enforce_type(tracker_type::tracker_int32);
#endif
    std::static_pointer_cast<tracker_element_int32>(e)->set(v);
}

template<> void SetTrackerValue(const shared_tracker_element& e, const uint64_t& v) {
#if TE_TYPE_SAFETY == 1
    e->enforce_type(tracker_type::tracker_uint64);
#endif
    std::static_pointer_cast<tracker_element_uint64>(e)->set(v);
}

template<> void SetTrackerValue(const shared_tracker_element& e, const int64_t& v) {
#if TE_TYPE_SAFETY == 1
    e->enforce_type(tracker_type::tracker_int64);
#endif
    std::static_pointer_cast<tracker_element_int64>(e)->set(v);
}

template<> void SetTrackerValue(const shared_tracker_element& e, const float& v) {
#if TE_TYPE_SAFETY == 1
    e->enforce_type(tracker_type::tracker_float);
#endif
    std::static_pointer_cast<tracker_element_float>(e)->set(v);
}

template<> void SetTrackerValue(const shared_tracker_element& e, const double& v) {
#if TE_TYPE_SAFETY == 1
    e->enforce_type(tracker_type::tracker_double);
#endif
    std::static_pointer_cast<tracker_element_double>(e)->set(v);
}

template<> void SetTrackerValue(const shared_tracker_element& e, const mac_addr& v) {
#if TE_TYPE_SAFETY == 1
    e->enforce_type(tracker_type::tracker_mac_addr);
#endif
    std::static_pointer_cast<tracker_element_mac_addr>(e)->set(v);
}

template<> void SetTrackerValue(const shared_tracker_element& e, const uuid& v) {
#if TE_TYPE_SAFETY == 1
    e->enforce_type(tracker_type::tracker_uuid);
#endif
    std::static_pointer_cast<tracker_element_uuid>(e)->set(v);
}

template<> void SetTrackerValue(const shared_tracker_element& e, const device_key& v) {
#if TE_TYPE_SAFETY == 1
    e->enforce_type(tracker_type::tracker_key);
#endif
    std::static_pointer_cast<tracker_element_device_key>(e)->set(v);
}

void tracker_element_serializer::pre_serialize_path(const SharedElementSummary& in_summary) {

    // Iterate through the path on this object, calling pre-serialize as
    // necessary on each object in the summary path

    shared_tracker_element inter = in_summary->parent_element;

    if (inter == nullptr)
        return;

    // Descend down the alias trail
    if (inter->get_type() == tracker_type::tracker_alias)
        inter = std::static_pointer_cast<tracker_element_alias>(inter)->get();

    try {
        for (auto p : in_summary->resolved_path) {
#if TE_TYPE_SAFETY == 1
            inter->enforce_type(tracker_type::tracker_map);
#endif

            inter = std::static_pointer_cast<tracker_element_map>(inter)->get_sub(p);

            if (inter == nullptr)
                return;

            // Descend down the alias trail
            if (inter->get_type() == tracker_type::tracker_alias)
                inter = std::static_pointer_cast<tracker_element_alias>(inter)->get();

            inter->pre_serialize();
        }
    } catch (std::runtime_error& c) {
        // Do nothing if we hit a map error
        return;
    }
}

void tracker_element_serializer::post_serialize_path(const SharedElementSummary& in_summary) {

    // Iterate through the path on this object, calling pre-serialize as
    // necessary on each object in the summary path

    shared_tracker_element inter = in_summary->parent_element;

    if (inter == nullptr)
        return;

    // Descend down the alias trail
    if (inter->get_type() == tracker_type::tracker_alias)
        inter = std::static_pointer_cast<tracker_element_alias>(inter)->get();

    try {
        for (auto p : in_summary->resolved_path) {
#if TE_TYPE_SAFETY == 1
            inter->enforce_type(tracker_type::tracker_map);
#endif

            inter = std::static_pointer_cast<tracker_element_map>(inter)->get_sub(p);

            if (inter == nullptr)
                return;

            // Descend down the alias trail
            if (inter->get_type() == tracker_type::tracker_alias)
                inter = std::static_pointer_cast<tracker_element_alias>(inter)->get();

            inter->post_serialize();
        }
    } catch (std::runtime_error& c) {
        // Do nothing if we hit a map error
        return;
    }
}

tracker_element_summary::tracker_element_summary(const SharedElementSummary& in_c) {
    parent_element = in_c->parent_element;
    resolved_path = in_c->resolved_path;
    rename = in_c->rename;
}

tracker_element_summary::tracker_element_summary(const std::string& in_path, 
        const std::string& in_rename) {
    parse_path(str_tokenize(in_path, "/"), in_rename);
}

tracker_element_summary::tracker_element_summary(const std::vector<std::string>& in_path,
        const std::string& in_rename) {
    parse_path(in_path, in_rename);
}

tracker_element_summary::tracker_element_summary(const std::string& in_path) {
    parse_path(str_tokenize(in_path, "/"), "");
}

tracker_element_summary::tracker_element_summary(const std::vector<std::string>& in_path) {
    parse_path(in_path, "");
}

tracker_element_summary::tracker_element_summary(const std::vector<int>& in_path,
        const std::string& in_rename) {
    resolved_path = in_path;
    rename = in_rename;
}

tracker_element_summary::tracker_element_summary(const std::vector<int>& in_path) {
    resolved_path = in_path;
}

void tracker_element_summary::parse_path(const std::vector<std::string>& in_path, 
        const std::string& in_rename) {

    if (in_path.size() == 0) {
        return;
    }

    bool path_full = true;

    for (auto pe : in_path) {
        if (pe.length() == 0)
            continue;

        auto id = Globalreg::globalreg->entrytracker->get_field_id(pe);

        if (id < 0)
            path_full = false;

        resolved_path.push_back(id);
    }

    if (!path_full) {
        rename = in_path[in_path.size() - 1];
    } else {
        rename = in_rename;
    }
}

shared_tracker_element get_tracker_element_path(const std::string& in_path, 
        shared_tracker_element elem) {
    return get_tracker_element_path(str_tokenize(in_path, "/"), elem);
}

shared_tracker_element get_tracker_element_path(const std::vector<std::string>& in_path, 
        shared_tracker_element elem) {

    if (in_path.size() < 1)
        return nullptr;

    if (elem == nullptr)
        return nullptr;

    shared_tracker_element next_elem;

    for (auto pe : in_path) {
        // Skip empty
        if (pe.length() == 0)
            continue;

        auto id = Globalreg::globalreg->entrytracker->get_field_id(pe);

        if (id < 0)
            return nullptr;

        if (next_elem == nullptr) {
#if TE_TYPE_SAFETY == 1
            elem->enforce_type(tracker_type::tracker_map);
#endif
            next_elem = std::static_pointer_cast<tracker_element_map>(elem)->get_sub(id);
        } else {
#if TE_TYPE_SAFETY == 1
            next_elem->enforce_type(tracker_type::tracker_map);
#endif
            next_elem = std::static_pointer_cast<tracker_element_map>(next_elem)->get_sub(id);
        }

        if (next_elem == nullptr)
            return nullptr;

    }

    return next_elem;
}

shared_tracker_element get_tracker_element_path(const std::vector<int>& in_path, 
        shared_tracker_element elem) {

    if (in_path.size() < 1)
        return nullptr;

    if (elem == nullptr)
        return nullptr;

    shared_tracker_element next_elem;

    for (auto pe : in_path) {
        if (pe < 0)
            return nullptr;

        if (next_elem == nullptr) {
#if TE_TYPE_SAFETY == 1
            elem->enforce_type(tracker_type::tracker_map);
#endif
            next_elem = std::static_pointer_cast<tracker_element_map>(elem)->get_sub(pe);
        } else {
            // Descend down the alias trail
            if (next_elem->get_type() == tracker_type::tracker_alias)
                next_elem = std::static_pointer_cast<tracker_element_alias>(next_elem)->get();

#if TE_TYPE_SAFETY == 1
            next_elem->enforce_type(tracker_type::tracker_map);
#endif
            next_elem = std::static_pointer_cast<tracker_element_map>(next_elem)->get_sub(pe);
        }

        if (next_elem == nullptr)
            return nullptr;

    }

    return next_elem;
}

std::vector<shared_tracker_element> get_tracker_element_multi_path(const std::string& in_path, 
        shared_tracker_element elem) {
    return get_tracker_element_multi_path(str_tokenize(in_path, "/"), elem);
}

std::vector<shared_tracker_element> get_tracker_element_multi_path(const std::vector<std::string>& in_path, 
        shared_tracker_element elem) {

    std::vector<shared_tracker_element> ret;

    if (in_path.size() < 1)
        return ret;

    shared_tracker_element next_elem = NULL;

    bool complex_fulfilled = false;

    // Descend down the alias trail
    if (elem->get_type() == tracker_type::tracker_alias)
        elem = std::static_pointer_cast<tracker_element_alias>(elem)->get();

    for (auto x = in_path.begin(); x != in_path.end(); ++x) {
        // Skip empty path element
        if (x->length() == 0)
            continue;

        auto id = Globalreg::globalreg->entrytracker->get_field_id(*x);

        if (id < 0) {
            return ret;
        }

        if (next_elem == nullptr) {
#if TE_TYPE_SAFETY == 1
            elem->enforce_type(tracker_type::tracker_map);
#endif
            next_elem = std::static_pointer_cast<tracker_element_map>(elem)->get_sub(id);
        } else {
            // Descend down the alias trail
            if (next_elem->get_type() == tracker_type::tracker_alias)
                next_elem = std::static_pointer_cast<tracker_element_alias>(next_elem)->get();

#if TE_TYPE_SAFETY == 1
            next_elem->enforce_type(tracker_type::tracker_map);
#endif
            next_elem = std::static_pointer_cast<tracker_element_map>(next_elem)->get_sub(id);
        }

        if (next_elem == nullptr) {
            return ret;
        }

        // If we're at the termination of the path, we just return the
        // object.  If we're in the middle of a path, we iterate over the 
        // contents of the container, and find the rest of the path in it
        if (x != std::next(in_path.end(), -1)) {
            auto type = next_elem->get_type();

            if (type == tracker_type::tracker_vector) {
                std::vector<std::string> sub_path(std::next(x, 1), in_path.end());

                auto cn = std::static_pointer_cast<tracker_element_vector>(next_elem);

                for (auto i : *cn) {
                    std::vector<shared_tracker_element> subret =
                        get_tracker_element_multi_path(sub_path, i);

                    ret.insert(ret.end(), subret.begin(), subret.end());
                }

                complex_fulfilled = true;
                break;
            } else if (type == tracker_type::tracker_int_map) {
                std::vector<std::string> sub_path(std::next(x, 1), in_path.end());

                auto cn = std::static_pointer_cast<tracker_element_int_map>(next_elem);

                for (auto i : *cn) {
                    std::vector<shared_tracker_element> subret =
                        get_tracker_element_multi_path(sub_path, i.second);

                    ret.insert(ret.end(), subret.begin(), subret.end());
                }

                complex_fulfilled = true;
                break;
            } else if (type == tracker_type::tracker_string_map) {
                std::vector<std::string> sub_path(std::next(x, 1), in_path.end());

                auto cn = std::static_pointer_cast<tracker_element_string_map>(next_elem);

                for (auto i : *cn) {
                    std::vector<shared_tracker_element> subret =
                        get_tracker_element_multi_path(sub_path, i.second);

                    ret.insert(ret.end(), subret.begin(), subret.end());
                }

                complex_fulfilled = true;
                break;
            } else if (type == tracker_type::tracker_mac_map) {
                std::vector<std::string> sub_path(std::next(x, 1), in_path.end());

                auto cn = std::static_pointer_cast<tracker_element_mac_map>(next_elem);

                for (auto i : *cn) {
                    std::vector<shared_tracker_element> subret =
                        get_tracker_element_multi_path(sub_path, i.second);

                    ret.insert(ret.end(), subret.begin(), subret.end());
                }

                complex_fulfilled = true;
                break;
            } else if (type == tracker_type::tracker_double_map) {
                std::vector<std::string> sub_path(std::next(x, 1), in_path.end());

                auto cn = std::static_pointer_cast<tracker_element_double_map>(next_elem);

                for (auto i : *cn) {
                    std::vector<shared_tracker_element> subret =
                        get_tracker_element_multi_path(sub_path, i.second);

                    ret.insert(ret.end(), subret.begin(), subret.end());
                }

                complex_fulfilled = true;
                break;
            }
        }
    }

    if (!complex_fulfilled)
        ret.push_back(next_elem);

    return ret;
}

std::vector<shared_tracker_element> get_tracker_element_multi_path(const std::vector<int>& in_path, 
        shared_tracker_element elem) {

    std::vector<shared_tracker_element> ret;

    if (in_path.size() < 1)
        return ret;

    shared_tracker_element next_elem = nullptr;

    // Descend down the alias trail
    if (elem->get_type() == tracker_type::tracker_alias)
        elem = std::static_pointer_cast<tracker_element_alias>(elem)->get();

    bool complex_fulfilled = false;
    for (auto x = in_path.begin(); x != in_path.end(); ++x) {
        int id = *x;

        if (id < 0) {
            return ret;
        }

        if (next_elem == nullptr) {
#if TE_TYPE_SAFETY == 1
            elem->enforce_type(tracker_type::tracker_map);
#endif
            next_elem = std::static_pointer_cast<tracker_element_map>(elem)->get_sub(id);
        } else {
            // Descend down the alias trail
            if (next_elem->get_type() == tracker_type::tracker_alias)
                next_elem = std::static_pointer_cast<tracker_element_alias>(next_elem)->get();

#if TE_TYPE_SAFETY == 1
            next_elem->enforce_type(tracker_type::tracker_map);
#endif
            next_elem = std::static_pointer_cast<tracker_element_map>(next_elem)->get_sub(id);
        }

        if (next_elem == nullptr) {
            return ret;
        }

        // If we're at the termination of the path, we just return the
        // object.  If we're in the middle of a path, we iterate over the 
        // contents of the container, and find the rest of the path in it
        if (x != std::next(in_path.end(), -1)) {
            auto type = next_elem->get_type();

            if (type == tracker_type::tracker_vector) {
                std::vector<int> sub_path(std::next(x, 1), in_path.end());

                auto cn = std::static_pointer_cast<tracker_element_vector>(next_elem);

                for (auto i : *cn) {
                    std::vector<shared_tracker_element> subret =
                        get_tracker_element_multi_path(sub_path, i);

                    ret.insert(ret.end(), subret.begin(), subret.end());
                }

                complex_fulfilled = true;
                break;
            } else if (type == tracker_type::tracker_int_map) {
                std::vector<int> sub_path(std::next(x, 1), in_path.end());

                auto cn = std::static_pointer_cast<tracker_element_int_map>(next_elem);

                for (auto i : *cn) {
                    std::vector<shared_tracker_element> subret =
                        get_tracker_element_multi_path(sub_path, i.second);

                    ret.insert(ret.end(), subret.begin(), subret.end());
                }

                complex_fulfilled = true;
                break;
            } else if (type == tracker_type::tracker_string_map) {
                std::vector<int> sub_path(std::next(x, 1), in_path.end());

                auto cn = std::static_pointer_cast<tracker_element_string_map>(next_elem);

                for (auto i : *cn) {
                    std::vector<shared_tracker_element> subret =
                        get_tracker_element_multi_path(sub_path, i.second);

                    ret.insert(ret.end(), subret.begin(), subret.end());
                }

                complex_fulfilled = true;
                break;
            } else if (type == tracker_type::tracker_mac_map) {
                std::vector<int> sub_path(std::next(x, 1), in_path.end());

                auto cn = std::static_pointer_cast<tracker_element_mac_map>(next_elem);

                for (auto i : *cn) {
                    std::vector<shared_tracker_element> subret =
                        get_tracker_element_multi_path(sub_path, i.second);

                    ret.insert(ret.end(), subret.begin(), subret.end());
                }

                complex_fulfilled = true;
                break;
            } else if (type == tracker_type::tracker_double_map) {
                std::vector<int> sub_path(std::next(x, 1), in_path.end());

                auto cn = std::static_pointer_cast<tracker_element_double_map>(next_elem);

                for (auto i : *cn) {
                    std::vector<shared_tracker_element> subret =
                        get_tracker_element_multi_path(sub_path, i.second);

                    ret.insert(ret.end(), subret.begin(), subret.end());
                }

                complex_fulfilled = true;
                break;
            }
        }
    }

    if (!complex_fulfilled)
        ret.push_back(next_elem);

    return ret;
}

std::shared_ptr<tracker_element> summarize_tracker_element(shared_tracker_element in, 
        const std::vector<SharedElementSummary>& in_summarization, 
        std::shared_ptr<tracker_element_serializer::rename_map> rename_map) {

    if (in->get_type() == tracker_type::tracker_vector) {
        auto ret = std::make_shared<tracker_element_vector>();
        auto inv = std::static_pointer_cast<tracker_element_vector>(in);

        for (auto i : *inv) 
            ret->push_back(summarize_single_tracker_element(i, in_summarization, rename_map));

        return ret;
    }

    return summarize_single_tracker_element(in, in_summarization, rename_map);
}

std::shared_ptr<tracker_element> summarize_single_tracker_element(shared_tracker_element in, 
        const std::vector<SharedElementSummary>& in_summarization, 
        std::shared_ptr<tracker_element_serializer::rename_map> rename_map) {

    auto ret_elem = std::make_shared<tracker_element_map>();

    if (in == nullptr)
        return ret_elem;

    // Poke the pre-serialization function to update anything that needs updating before
    // we create the new meta-object
    in->pre_serialize();

    if (in_summarization.size() == 0) {
        in->post_serialize();
        return in;
    }

    unsigned int fn = 0;

    for (auto si = in_summarization.begin(); si != in_summarization.end(); ++si) {
        fn++;

        if ((*si)->resolved_path.size() == 0)
            continue;

        shared_tracker_element f =
            get_tracker_element_path((*si)->resolved_path, in);

        if (f == NULL) {
            f = Globalreg::globalreg->entrytracker->register_and_get_field("unknown" + int_to_string(fn),
                    tracker_element_factory<tracker_element_int8>(),
                    "unallocated field");

            std::static_pointer_cast<tracker_element_int8>(f)->set(0);
        
            if ((*si)->rename.length() != 0) {
                f->set_local_name((*si)->rename);
            } else {
                // Get the last name of the field in the path, if we can...
                int lastid = (*si)->resolved_path[(*si)->resolved_path.size() - 1];

                if (lastid < 0)
                    f->set_local_name("unknown" + int_to_string(fn));
                else
                    f->set_local_name(Globalreg::globalreg->entrytracker->get_field_name(lastid));
            }
        } 

       
        // If we're renaming it or we're a path, we put the record in.  We need
        // to duplicate the summary object and make a reference to our parent
        // object so that when we serialize we can descend the path calling
        // the proper pre-serialization methods
        if ((*si)->rename.length() != 0 || (*si)->resolved_path.size() > 1) {
            auto sum = std::make_shared<tracker_element_summary>(*si);
            sum->parent_element = in;
            (*rename_map)[f] = sum;
        }

        std::static_pointer_cast<tracker_element_map>(ret_elem)->insert(f);
    }

    in->post_serialize();

    return ret_elem;
}

bool sort_tracker_element_less(const std::shared_ptr<tracker_element> lhs, 
        const std::shared_ptr<tracker_element> rhs) {

    // Only allow equal compares
    if (lhs->get_type() != rhs->get_type())
        throw std::runtime_error(fmt::format("Attempted to compare two non-equal field types, "
                    "{} < {}", lhs->get_type_as_string(), rhs->get_type_as_string()));

    switch (lhs->get_type()) {
        case tracker_type::tracker_string:
            return tracker_element::safe_cast_as<tracker_element_string>(lhs)->less_than(*tracker_element::safe_cast_as<tracker_element_string>(rhs));
        case tracker_type::tracker_int8:
            return tracker_element::safe_cast_as<tracker_element_int8>(lhs)->less_than(*tracker_element::safe_cast_as<tracker_element_int8>(rhs));
        case tracker_type::tracker_uint8:
            return tracker_element::safe_cast_as<tracker_element_uint8>(lhs)->less_than(*tracker_element::safe_cast_as<tracker_element_uint8>(rhs));
        case tracker_type::tracker_int16:
            return tracker_element::safe_cast_as<tracker_element_int16>(lhs)->less_than(*tracker_element::safe_cast_as<tracker_element_int16>(rhs));
        case tracker_type::tracker_uint16:
            return tracker_element::safe_cast_as<tracker_element_uint16>(lhs)->less_than(*tracker_element::safe_cast_as<tracker_element_uint16>(rhs));
        case tracker_type::tracker_int32:
            return tracker_element::safe_cast_as<tracker_element_int32>(lhs)->less_than(*tracker_element::safe_cast_as<tracker_element_int32>(rhs));
        case tracker_type::tracker_uint32:
            return tracker_element::safe_cast_as<tracker_element_uint32>(lhs)->less_than(*tracker_element::safe_cast_as<tracker_element_uint32>(rhs));
        case tracker_type::tracker_int64:
            return tracker_element::safe_cast_as<tracker_element_int64>(lhs)->less_than(*tracker_element::safe_cast_as<tracker_element_int64>(rhs));
        case tracker_type::tracker_uint64:
            return tracker_element::safe_cast_as<tracker_element_uint64>(lhs)->less_than(*tracker_element::safe_cast_as<tracker_element_uint64>(rhs));
        case tracker_type::tracker_float:
            return tracker_element::safe_cast_as<tracker_element_float>(lhs)->less_than(*tracker_element::safe_cast_as<tracker_element_float>(rhs));
        case tracker_type::tracker_double:
            return tracker_element::safe_cast_as<tracker_element_double>(lhs)->less_than(*tracker_element::safe_cast_as<tracker_element_double>(rhs));
        case tracker_type::tracker_mac_addr:
            return tracker_element::safe_cast_as<tracker_element_mac_addr>(lhs)->less_than(*tracker_element::safe_cast_as<tracker_element_mac_addr>(rhs));
        case tracker_type::tracker_uuid:
            return tracker_element::safe_cast_as<tracker_element_uuid>(lhs)->less_than(*tracker_element::safe_cast_as<tracker_element_uuid>(rhs));
        case tracker_type::tracker_byte_array:
            return tracker_element::safe_cast_as<tracker_element_byte_array>(lhs)->less_than(*tracker_element::safe_cast_as<tracker_element_byte_array>(rhs));
        case tracker_type::tracker_key:
        case tracker_type::tracker_vector:
        case tracker_type::tracker_map:
        case tracker_type::tracker_int_map:
        case tracker_type::tracker_mac_map:
        case tracker_type::tracker_string_map:
        case tracker_type::tracker_double_map:
        case tracker_type::tracker_key_map:
        case tracker_type::tracker_vector_double:
        case tracker_type::tracker_double_map_double:
        case tracker_type::tracker_vector_string:
        case tracker_type::tracker_hashkey_map:
        case tracker_type::tracker_alias:
            throw std::runtime_error(fmt::format("Attempted to compare a complex field type, {}",
                        lhs->get_type_as_string()));
    }

    return false;
}

bool fast_sort_tracker_element_less(const std::shared_ptr<tracker_element> lhs, 
        const std::shared_ptr<tracker_element> rhs) noexcept {

    switch (lhs->get_type()) {
        case tracker_type::tracker_string:
            return std::static_pointer_cast<tracker_element_string>(lhs)->less_than(*std::static_pointer_cast<tracker_element_string>(rhs));
        case tracker_type::tracker_int8:
            return std::static_pointer_cast<tracker_element_int8>(lhs)->less_than(*std::static_pointer_cast<tracker_element_int8>(rhs));
        case tracker_type::tracker_uint8:
            return std::static_pointer_cast<tracker_element_uint8>(lhs)->less_than(*std::static_pointer_cast<tracker_element_uint8>(rhs));
        case tracker_type::tracker_int16:
            return std::static_pointer_cast<tracker_element_int16>(lhs)->less_than(*std::static_pointer_cast<tracker_element_int16>(rhs));
        case tracker_type::tracker_uint16:
            return std::static_pointer_cast<tracker_element_uint16>(lhs)->less_than(*std::static_pointer_cast<tracker_element_uint16>(rhs));
        case tracker_type::tracker_int32:
            return std::static_pointer_cast<tracker_element_int32>(lhs)->less_than(*std::static_pointer_cast<tracker_element_int32>(rhs));
        case tracker_type::tracker_uint32:
            return std::static_pointer_cast<tracker_element_uint32>(lhs)->less_than(*std::static_pointer_cast<tracker_element_uint32>(rhs));
        case tracker_type::tracker_int64:
            return std::static_pointer_cast<tracker_element_int64>(lhs)->less_than(*std::static_pointer_cast<tracker_element_int64>(rhs));
        case tracker_type::tracker_uint64:
            return std::static_pointer_cast<tracker_element_uint64>(lhs)->less_than(*std::static_pointer_cast<tracker_element_uint64>(rhs));
        case tracker_type::tracker_float:
            return std::static_pointer_cast<tracker_element_float>(lhs)->less_than(*std::static_pointer_cast<tracker_element_float>(rhs));
        case tracker_type::tracker_double:
            return std::static_pointer_cast<tracker_element_double>(lhs)->less_than(*std::static_pointer_cast<tracker_element_double>(rhs));
        case tracker_type::tracker_mac_addr:
            return std::static_pointer_cast<tracker_element_mac_addr>(lhs)->less_than(*std::static_pointer_cast<tracker_element_mac_addr>(rhs));
        case tracker_type::tracker_uuid:
            return std::static_pointer_cast<tracker_element_uuid>(lhs)->less_than(*std::static_pointer_cast<tracker_element_uuid>(rhs));
        case tracker_type::tracker_byte_array:
            return std::static_pointer_cast<tracker_element_byte_array>(lhs)->less_than(*std::static_pointer_cast<tracker_element_byte_array>(rhs));
        case tracker_type::tracker_key:
        case tracker_type::tracker_vector:
        case tracker_type::tracker_map:
        case tracker_type::tracker_int_map:
        case tracker_type::tracker_mac_map:
        case tracker_type::tracker_string_map:
        case tracker_type::tracker_double_map:
        case tracker_type::tracker_key_map:
        case tracker_type::tracker_vector_double:
        case tracker_type::tracker_double_map_double:
        case tracker_type::tracker_vector_string:
        case tracker_type::tracker_hashkey_map:
        case tracker_type::tracker_alias:
            return false;
    }

    return false;
}

