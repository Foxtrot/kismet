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

#include "kis_datasource.h"
#include "simple_datasource_proto.h"
#include "endian_magic.h"
#include "configfile.h"
#include "msgpack_adapter.h"

// We never instantiate from a generic tracker component or from a stored
// record so we always re-allocate ourselves
KisDatasource::KisDatasource(GlobalRegistry *in_globalreg, 
        SharedDatasourceBuilder in_builder) :
    tracker_component(in_globalreg, 0) {

    pthread_mutexattr_t mutexattr;
    pthread_mutexattr_init(&mutexattr);
    pthread_mutexattr_settype(&mutexattr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&source_lock, &mutexattr);

    globalreg = in_globalreg;
    
    register_fields();
    reserve_fields(NULL);

    set_source_builder(in_builder);

    timetracker = 
        static_pointer_cast<Timetracker>(globalreg->FetchGlobal("TIMETRACKER"));

    packetchain =
        static_pointer_cast<Packetchain>(globalreg->FetchGlobal("PACKETCHAIN"));

	pack_comp_linkframe = packetchain->RegisterPacketComponent("LINKFRAME");
    pack_comp_l1info = packetchain->RegisterPacketComponent("RADIODATA");
    pack_comp_gps = packetchain->RegisterPacketComponent("GPS");

    next_cmd_sequence = rand(); 

    error_timer_id = -1;
}

KisDatasource::~KisDatasource() {
    local_eol_locker lock(&source_lock);

    close_source();

    pthread_mutex_destroy(&source_lock);
}

void KisDatasource::list_interfaces(unsigned int in_transaction, 
        list_callback_t in_cb) {
    local_locker lock(&source_lock);

    // If we can't list interfaces according to our prototype, die 
    // and call the cb instantly
    if (!get_source_builder()->get_list_capable()) {
        if (in_cb != NULL) {
            in_cb(in_transaction, vector<SharedInterface>());
        }

        return;
    }

    // Otherwise create and send a list command
    send_command_list_interfaces(in_transaction, in_cb);
}

void KisDatasource::probe_interface(string in_definition, unsigned int in_transaction,
        probe_callback_t in_cb) {
    local_locker lock(&source_lock);
    
    // If we can't probe interfaces according to our prototype, die
    // and call the cb instantly
    if (!get_source_builder()->get_probe_capable()) {
        if (in_cb != NULL) {
            in_cb(in_transaction, false, "Driver not capable of probing");
        }

        return;
    }

    // Create and send list command
    send_command_probe_interface(in_definition, in_transaction, in_cb);
}

void KisDatasource::open_interface(string in_definition, unsigned int in_transaction, 
        open_callback_t in_cb) {
    local_locker lock(&source_lock);
    
    // If we can't open local interfaces, die
    if (!get_source_builder()->get_local_capable()) {
        if (in_cb != NULL) {
            in_cb(in_transaction, false, "Driver does not support direct capture");
        }
        
        return;
    }

    // If we have an error callback that's going to try to re-open us, cancel it
    if (error_timer_id > 0)
        timetracker->RemoveTimer(error_timer_id);

    // Populate our local info about the interface
    if (!parse_interface_definition(in_definition)) {
        if (in_cb != NULL) {
            in_cb(in_transaction, false, "Malformed source config");
        }

        return;
    }

    // Create and send open command
    send_command_open_interface(in_definition, in_transaction, in_cb);
}

void KisDatasource::set_channel(string in_channel, unsigned int in_transaction,
        configure_callback_t in_cb) {
    local_locker lock(&source_lock);

    if (!get_source_builder()->get_tune_capable()) {
        if (in_cb != NULL) {
            in_cb(in_transaction, false, "Driver not capable of changing channel");
        }
        return;
    }

    send_command_set_channel(in_channel, in_transaction, in_cb);
}

void KisDatasource::set_channel_hop(double in_rate, std::vector<std::string> in_chans,
        unsigned int in_transaction, configure_callback_t in_cb) {
    local_locker lock(&source_lock);

    if (!get_source_builder()->get_tune_capable()) {
        if (in_cb != NULL) {
            in_cb(in_transaction, false, "Driver not capable of changing channel");
        }
        return;
    }

    // Convert the std::vector to a channel vector
    SharedTrackerElement elem(get_source_hop_vec()->clone_type());
    TrackerElementVector vec(elem);

    for (auto i = in_chans.begin(); i != in_chans.end(); ++i) {
        SharedTrackerElement c(channel_entry_builder->clone_type());
        c->set(*i);
        vec.push_back(c);
    }

    // Call the tracker element variation
    set_channel_hop(in_rate, elem, in_transaction, in_cb);
}

void KisDatasource::set_channel_hop(double in_rate, SharedTrackerElement in_chans,
        unsigned int in_transaction, configure_callback_t in_cb) {
    local_locker lock(&source_lock);

    if (!get_source_builder()->get_tune_capable()) {
        if (in_cb != NULL) {
            in_cb(in_transaction, false, "Driver not capable of changing channel");
        }
        return;
    }

    // Generate the command and send it
    send_command_set_channel_hop(in_rate, in_chans, in_transaction, in_cb);
}

void KisDatasource::set_channel_hop_rate(double in_rate, unsigned int in_transaction,
        configure_callback_t in_cb) {
    // Don't bother checking if we can set channel since we're just calling a function
    // that already checks that
    set_channel_hop(in_rate, get_source_hop_vec(), in_transaction, in_cb);
}

void KisDatasource::set_channel_hop_list(std::vector<std::string> in_chans,
        unsigned int in_transaction, configure_callback_t in_cb) {
    // Again don't bother, we're just an API shim
    set_channel_hop(get_source_hop_rate(), in_chans, in_transaction, in_cb);
}

void KisDatasource::connect_ringbuffer(shared_ptr<RingbufferHandler> in_ringbuf) {
    local_locker lock(&source_lock);
    // Assign the ringbuffer & set us as the wakeup interface
    ringbuf_handler = in_ringbuf;
    ringbuf_handler->SetReadBufferInterface(this);
}

void KisDatasource::close_source() {
    local_locker lock(&source_lock);

    if (error_timer_id > 0)
        timetracker->RemoveTimer(error_timer_id);

    cancel_all_commands("Closing source");

    // Common close via ringbuf_handler; will call the IPC or TCP server close
    if (ringbuf_handler != NULL)
        ringbuf_handler->CloseHandler("Closing source");
}

void KisDatasource::BufferAvailable(size_t in_amt) {
    // Handle reading raw frames off the incoming buffer and validate their
    // framing, then break them into KVMap records and dispatch them.
    //
    // We can survive unknown frame types, but we can't survive invalid ones -
    // if we get an invalid frame, throw an error and drop into the error
    // processing.
    
    local_locker lock(&source_lock);
    
    simple_cap_proto_t *frame_header;
    uint8_t *buf;
    uint32_t frame_sz;
    uint32_t frame_checksum, calc_checksum;

    if (in_amt < sizeof(simple_cap_proto_t)) {
        return;
    }

    // Peek the buffer
    buf = new uint8_t[in_amt];
    ringbuf_handler->PeekReadBufferData(buf, in_amt);

    frame_header = (simple_cap_proto_t *) buf;

    if (kis_ntoh32(frame_header->signature) != KIS_CAP_SIMPLE_PROTO_SIG) {
        delete[] buf;

        _MSG("Kismet data source " + get_source_name() + " got an invalid "
                "control from on IPC/Network, closing.", MSGFLAG_ERROR);
        trigger_error("Source got invalid control frame");

        return;
    }

    frame_sz = kis_ntoh32(frame_header->packet_sz);

    if (frame_sz > in_amt) {
        // Nothing we can do right now, not enough data to make up a complete packet.
        delete[] buf;
        return;
    }

    // Get the checksum & save it
    frame_checksum = kis_ntoh32(frame_header->checksum);

    // Zero the checksum field in the packet
    frame_header->checksum = 0x00000000;

    // Calc the checksum of the rest
    calc_checksum = Adler32Checksum((const char *) buf, frame_sz);

    // Compare to the saved checksum
    if (calc_checksum != frame_checksum) {
        delete[] buf;

        _MSG("Kismet data source " + get_source_name() + " got an invalid checksum "
                "on control from IPC/Network, closing.", MSGFLAG_ERROR);
        trigger_error("Source got invalid control frame");

        return;
    }

    // Consume the packet in the ringbuf 
    ringbuf_handler->GetReadBufferData(NULL, frame_sz);

    // Extract the kv pairs
    KVmap kv_map;

    ssize_t data_offt = 0;
    for (unsigned int kvn = 0; kvn < kis_ntoh32(frame_header->num_kv_pairs); kvn++) {
        simple_cap_proto_kv *pkv =
            (simple_cap_proto_kv *) &((frame_header->data)[data_offt]);

        data_offt = 
            sizeof(simple_cap_proto_kv_h_t) +
            kis_ntoh32(pkv->header.obj_sz);

        KisDatasourceCapKeyedObject *kv =
            new KisDatasourceCapKeyedObject(pkv);

        kv_map[StrLower(kv->key)] = kv;
    }

    char ctype[17];
    snprintf(ctype, 17, "%s", frame_header->type);
    proto_dispatch_packet(ctype, kv_map);

    for (auto i = kv_map.begin(); i != kv_map.end(); ++i) {
        delete i->second;
    }

    delete[] buf;
}

void KisDatasource::BufferError(string in_error) {
    // Simple passthrough to crash the source out from an error at the buffer level
    trigger_error(in_error);
}

void KisDatasource::trigger_error(string in_error) {
    local_locker lock(&source_lock);

    // Something has gone wrong; we need to cancel all pending commands
    cancel_all_commands(in_error);

    // And shut down the RB handler, which will kill whatever
    if (ringbuf_handler != NULL)
        ringbuf_handler->ErrorHandler(in_error);

    set_int_source_error(true);
    set_int_source_error_reason(in_error);
}

bool KisDatasource::parse_interface_definition(string in_definition) {
    local_locker lock(&source_lock);

    local_uuid = false;

    string interface;

    size_t cpos = in_definition.find(":");

    // If there's no ':' then there are no options
    if (cpos == string::npos) {
        set_int_source_interface(in_definition);
        set_source_name(in_definition);
        return true;
    }

    // Slice the interface
    set_int_source_interface(in_definition.substr(0, cpos));

    // Turn the rest into an opt vector
    std::vector<opt_pair> options;

    // Blow up if we fail parsing
    if (StringToOpts(in_definition.substr(cpos + 1, 
                in_definition.size() - cpos - 1), ",", &options) < 0) {
        return false;
    }

    // Throw into a nice keyed dictionary so other elements of the DS can use it
    for (auto i = options.begin(); i != options.end(); ++i) {
        source_definition_opts[StrLower((*i).opt)] = (*i).val;
    }

    // Set some basic options
    
    auto name_i = source_definition_opts.find("name");

    if (name_i != source_definition_opts.end()) {
        set_source_name(name_i->second);
    } else {
        set_source_name(get_source_interface());
    }

    auto uuid_i = source_definition_opts.find("uuid");
    if (uuid_i != source_definition_opts.end()) {
        uuid u(uuid_i->second);

        if (u.error) {
            _MSG("Invalid UUID for data source " + get_source_name() + "/" + 
                    get_source_interface(), MSGFLAG_ERROR);
            return false;
        }

        set_source_uuid(u);
        local_uuid = true;
    }

    auto error_i = source_definition_opts.find("retry");
    if (error_i != source_definition_opts.end()) {
        set_int_source_retry(StringToBool(error_i->second, true));
    }
   
    return true;
}

KisDatasource::tracked_command *KisDatasource::get_command(uint32_t in_transaction) {
    auto i = command_ack_map.find(in_transaction);

    if (i == command_ack_map.end())
        return NULL;

    return i->second;
}

void KisDatasource::cancel_command(uint32_t in_transaction, string in_error) {
    local_locker lock(&source_lock);

    auto i = command_ack_map.find(in_transaction);
    if (i != command_ack_map.end()) {
        // Cancel any callbacks
        if (i->second->list_cb != NULL) {
            i->second->list_cb(i->second->transaction, vector<SharedInterface>());
        } else if (i->second->probe_cb != NULL) {
            i->second->probe_cb(i->second->transaction, false, in_error);
        } else if (i->second->open_cb != NULL) {
            i->second->open_cb(i->second->transaction, false, in_error);
        } else if (i->second->configure_cb != NULL) {
            i->second->configure_cb(i->second->transaction, false, in_error);
        }

        // Cancel any timers
        if (i->second->timer_id > -1) {
            timetracker->RemoveTimer(i->second->timer_id);
        }

        delete(i->second);
        command_ack_map.erase(i);
    }
}

void KisDatasource::cancel_all_commands(string in_error) {
    local_locker lock(&source_lock);

    auto i = command_ack_map.begin();

    while (i != command_ack_map.end())
        cancel_command(i->first, in_error);
}

void KisDatasource::proto_dispatch_packet(string in_type, KVmap in_kvmap) {
    string ltype = StrLower(in_type);

    if (ltype == "proberesp")
        proto_packet_probe_resp(in_kvmap);
    else if (ltype == "openresp")
        proto_packet_open_resp(in_kvmap);
    else if (ltype == "listresp")
        proto_packet_list_resp(in_kvmap);
    else if (ltype == "error")
        proto_packet_error(in_kvmap);
    else if (ltype == "message")
        proto_packet_message(in_kvmap);
    else if (ltype == "configresp")
        proto_packet_configresp(in_kvmap);
    else if (ltype == "data")
        proto_packet_data(in_kvmap);

    // We don't care about types we don't understand
}

void KisDatasource::proto_packet_probe_resp(KVmap in_kvpairs) {
    KVmap::iterator i;
    string msg;

    // Process any messages
    if ((i = in_kvpairs.find("message")) != in_kvpairs.end()) {
        msg = handle_kv_message(i->second);
    }

    // Process channels list if we got one; this will populate our
    // channels fields automatically
    if ((i = in_kvpairs.find("channels")) != in_kvpairs.end()) {
        handle_kv_channels(i->second);
    }

    // Process single channel if we got one
    if ((i = in_kvpairs.find("chanset")) != in_kvpairs.end()) {
        handle_kv_config_channel(i->second);
    }

    // If we don't have a success record we're flat out invalid
    if ((i = in_kvpairs.find("success")) == in_kvpairs.end()) {
        trigger_error("No valid response found for probe request");
        return;
    }

    // Get the sequence number and look up our command
    uint32_t seq = get_kv_success_sequence(i->second);
    auto ci = command_ack_map.find(seq);
    if (ci != command_ack_map.end()) {
        if (ci->second->probe_cb != NULL)
            ci->second->probe_cb(seq, get_kv_success(i->second), msg);
        delete(ci->second);
        command_ack_map.erase(ci);
    }

    // Close down the source when we're done probing
    close_source();
}

void KisDatasource::proto_packet_open_resp(KVmap in_kvpairs) {
    KVmap::iterator i;
    string msg;

    // Process any messages
    if ((i = in_kvpairs.find("message")) != in_kvpairs.end()) {
        msg = handle_kv_message(i->second);
    }

    // Process channels list if we got one
    if ((i = in_kvpairs.find("channels")) != in_kvpairs.end()) {
        handle_kv_channels(i->second);
    }

    // Process config list
    if ((i = in_kvpairs.find("chanset")) != in_kvpairs.end()) {
        handle_kv_config_channel(i->second);
    }

    if ((i = in_kvpairs.find("chanhop")) != in_kvpairs.end()) {
        handle_kv_config_hop(i->second);
    }

    if ((i = in_kvpairs.find("uuid")) != in_kvpairs.end()) {
        handle_kv_uuid(i->second);
    }

    // If we don't have a success record we're flat out invalid
    if ((i = in_kvpairs.find("success")) == in_kvpairs.end()) {
        trigger_error("No valid response found for open request");
        return;
    }

    // Get the sequence number and look up our command
    uint32_t seq = get_kv_success_sequence(i->second);
    auto ci = command_ack_map.find(seq);
    if (ci != command_ack_map.end()) {
        if (ci->second->open_cb != NULL)
            ci->second->open_cb(seq, get_kv_success(i->second), msg);
        delete(ci->second);
        command_ack_map.erase(ci);
    }

    // If the open failed, kill the source
    if (!get_kv_success(i->second)) {
        trigger_error(msg);
    }

}

void KisDatasource::proto_packet_list_resp(KVmap in_kvpairs) {
    KVmap::iterator i;
    string msg;

    if ((i = in_kvpairs.find("message")) != in_kvpairs.end()) {
        msg = handle_kv_message(i->second);
    }

    if ((i = in_kvpairs.find("interfacelist")) != in_kvpairs.end()) {
        handle_kv_interfacelist(i->second);
    }

    // If we don't have a success record we're flat out invalid
    if ((i = in_kvpairs.find("success")) == in_kvpairs.end()) {
        trigger_error("No valid response found for list request");
        return;
    }

    // Get the sequence number and look up our command
    uint32_t seq = get_kv_success_sequence(i->second);
    auto ci = command_ack_map.find(seq);
    if (ci != command_ack_map.end()) {
        if (ci->second->list_cb != NULL)
            ci->second->list_cb(seq, listed_interfaces);
        delete(ci->second);
        command_ack_map.erase(ci);
    }

    // We're done after listing
    close_source();
}

void KisDatasource::proto_packet_error(KVmap in_kvpairs) {
    KVmap::iterator i;

    string fail_reason = "Received error frame on data source";

    // Process any messages
    if ((i = in_kvpairs.find("message")) != in_kvpairs.end()) {
        fail_reason = handle_kv_message(i->second);
    }

    trigger_error(fail_reason);
}

void KisDatasource::proto_packet_message(KVmap in_kvpairs) {
    KVmap::iterator i;

    // Process any messages
    if ((i = in_kvpairs.find("message")) != in_kvpairs.end()) {
        handle_kv_message(i->second);
    }
}

void KisDatasource::proto_packet_configresp(KVmap in_kvpairs) {
    KVmap::iterator i;
    string msg;

    // Process any messages
    if ((i = in_kvpairs.find("message")) != in_kvpairs.end()) {
        msg = handle_kv_message(i->second);
    }

    // Process config list
    if ((i = in_kvpairs.find("chanset")) != in_kvpairs.end()) {
        handle_kv_config_channel(i->second);
    }

    if ((i = in_kvpairs.find("chanhop")) != in_kvpairs.end()) {
        handle_kv_config_hop(i->second);
    }

    // If we don't have a success record we're flat out invalid
    if ((i = in_kvpairs.find("success")) == in_kvpairs.end()) {
        trigger_error("No valid response found for config request");
        return;
    }

    // Get the sequence number and look up our command
    uint32_t seq = get_kv_success_sequence(i->second);
    auto ci = command_ack_map.find(seq);
    if (ci != command_ack_map.end()) {
        if (ci->second->configure_cb != NULL)
            ci->second->configure_cb(seq, get_kv_success(i->second), msg);
        delete(ci->second);
        command_ack_map.erase(ci);
    }

    if (!get_kv_success(i->second)) {
        trigger_error(msg);
    }
}

void KisDatasource::proto_packet_data(KVmap in_kvpairs) {
    KVmap::iterator i;

    kis_packet *packet = NULL;
    kis_layer1_packinfo *siginfo = NULL;
    kis_gps_packinfo *gpsinfo = NULL;

    // Process any messages
    if ((i = in_kvpairs.find("message")) != in_kvpairs.end()) {
        handle_kv_message(i->second);
    }

    // Do we have a packet?
    if ((i = in_kvpairs.find("packet")) != in_kvpairs.end()) {
        packet = handle_kv_packet(i->second);
    }

    if (packet == NULL)
        return;

    // Gather signal data
    if ((i = in_kvpairs.find("signal")) != in_kvpairs.end()) {
        siginfo = handle_kv_signal(i->second);
    }
    
    // Gather GPS data
    if ((i = in_kvpairs.find("gps")) != in_kvpairs.end()) {
        gpsinfo = handle_kv_gps(i->second);
    }

    // Add them to the packet
    if (siginfo != NULL) {
        packet->insert(pack_comp_l1info, siginfo);
    }

    if (gpsinfo != NULL) {
        packet->insert(pack_comp_gps, gpsinfo);
    }

    // Inject the packet into the packetchain if we have one
    packetchain->ProcessPacket(packet);

}

bool KisDatasource::get_kv_success(KisDatasourceCapKeyedObject *in_obj) {
    if (in_obj->size != sizeof(simple_cap_proto_success_value)) {
        trigger_error("Invalid SUCCESS object in response");
        return false;
    }

    simple_cap_proto_success_t *status = (simple_cap_proto_success_t *) in_obj->object;

    return status->success;
}

uint32_t KisDatasource::get_kv_success_sequence(KisDatasourceCapKeyedObject *in_obj) {
    if (in_obj->size != sizeof(simple_cap_proto_success_value)) {
        trigger_error("Invalid SUCCESS object in response");
        return false;
    }

    simple_cap_proto_success_t *status = (simple_cap_proto_success_t *) in_obj->object;
    uint32_t seqno = kis_ntoh32(status->sequence_number);

    return seqno;
}

string KisDatasource::handle_kv_message(KisDatasourceCapKeyedObject *in_obj) {
    // Unpack the dictionary
    MsgpackAdapter::MsgpackStrMap dict;
    msgpack::unpacked result;
    MsgpackAdapter::MsgpackStrMap::iterator obj_iter;
    vector<string> channel_vec;
    string msg;

    try {
        msgpack::unpack(result, in_obj->object, in_obj->size); 
        msgpack::object deserialized = result.get();
        dict = deserialized.as<MsgpackAdapter::MsgpackStrMap>();

        unsigned int flags;

        if ((obj_iter = dict.find("msg")) != dict.end()) {
            msg = obj_iter->second.as<string>();
        } else {
            throw std::runtime_error("missing 'msg' entry");
        }

        if ((obj_iter = dict.find("flags")) != dict.end()) {
            flags = obj_iter->second.as<unsigned int>();
        } else {
            throw std::runtime_error("missing 'flags' entry");
        }

        _MSG(msg, flags);

    } catch (const std::exception& e) {
        // Something went wrong with msgpack unpacking
        stringstream ss;
        ss << "failed to unpack message bundle: " << e.what();

        trigger_error(ss.str());

        return ss.str();
    }

    return msg;
}

void KisDatasource::handle_kv_channels(KisDatasourceCapKeyedObject *in_obj) {
    // Extracts the keyed value from a msgpack dictionary, turns it into
    // a string vector, then clears our local channel list and populates it with
    // the new data sent to us; this lets us inherit the channel list
    // as a whole

    // Unpack the dictionary
    MsgpackAdapter::MsgpackStrMap dict;
    msgpack::unpacked result;
    MsgpackAdapter::MsgpackStrMap::iterator obj_iter;
    vector<string> channel_vec;

    try {
        msgpack::unpack(result, in_obj->object, in_obj->size);
        msgpack::object deserialized = result.get();
        dict = deserialized.as<MsgpackAdapter::MsgpackStrMap>();

        if ((obj_iter = dict.find("channels")) != dict.end()) {
            MsgpackAdapter::AsStringVector(obj_iter->second, channel_vec);

            // We now have a string vector of channels, dupe it into our 
            // tracked channels vec
            local_locker lock(&source_lock);

            TrackerElementVector chan_vec(get_int_source_channels_vec());
            chan_vec.clear();

            for (unsigned int x = 0; x < channel_vec.size(); x++) {
                SharedTrackerElement chanstr = 
                    channel_entry_builder->clone_type();
                chanstr->set(channel_vec[x]);
                chan_vec.push_back(chanstr);
            }
        }
    } catch (const std::exception& e) {
        // Something went wrong with msgpack unpacking
        stringstream ss;
        ss << "failed to unpack proberesp channels bundle: " << e.what();

        trigger_error(ss.str());

        return;
    }

    return;
}

kis_layer1_packinfo *KisDatasource::handle_kv_signal(KisDatasourceCapKeyedObject *in_obj) {
    // Extract l1 info from a KV pair so we can add it to a packet
    
    kis_layer1_packinfo *siginfo = new kis_layer1_packinfo();

    // Unpack the dictionary
    MsgpackAdapter::MsgpackStrMap dict;
    msgpack::unpacked result;
    MsgpackAdapter::MsgpackStrMap::iterator obj_iter;

    try {
        msgpack::unpack(result, in_obj->object, in_obj->size);
        msgpack::object deserialized = result.get();
        dict = deserialized.as<MsgpackAdapter::MsgpackStrMap>();

        if ((obj_iter = dict.find("signal_dbm")) != dict.end()) {
            siginfo->signal_type = kis_l1_signal_type_dbm;
            siginfo->signal_dbm = obj_iter->second.as<int32_t>();
        }

        if ((obj_iter = dict.find("noise_dbm")) != dict.end()) {
            siginfo->signal_type = kis_l1_signal_type_dbm;
            siginfo->noise_dbm = obj_iter->second.as<int32_t>();
        }

        if ((obj_iter = dict.find("signal_rssi")) != dict.end()) {
            siginfo->signal_type = kis_l1_signal_type_rssi;
            siginfo->signal_rssi = obj_iter->second.as<int32_t>();
        }

        if ((obj_iter = dict.find("noise_rssi")) != dict.end()) {
            siginfo->signal_type = kis_l1_signal_type_rssi;
            siginfo->noise_rssi = obj_iter->second.as<int32_t>();
        }

        if ((obj_iter = dict.find("freq_khz")) != dict.end()) {
            siginfo->freq_khz = obj_iter->second.as<double>();
        }

        if ((obj_iter = dict.find("channel")) != dict.end()) {
            siginfo->channel = obj_iter->second.as<string>();
        }

        if ((obj_iter = dict.find("datarate")) != dict.end()) {
            siginfo->datarate = obj_iter->second.as<double>();
        }

    } catch (const std::exception& e) {
        delete(siginfo);

        // Something went wrong with msgpack unpacking
        stringstream ss;
        ss << "failed to unpack gps bundle: " << e.what();

        trigger_error(ss.str());
        return NULL;
    }

    return siginfo;
}

kis_gps_packinfo *KisDatasource::handle_kv_gps(KisDatasourceCapKeyedObject *in_obj) {
    // Extract a GPS record from a packet and turn it into a packinfo gps log
    kis_gps_packinfo *gpsinfo = new kis_gps_packinfo();

    // Unpack the dictionary
    MsgpackAdapter::MsgpackStrMap dict;
    msgpack::unpacked result;
    MsgpackAdapter::MsgpackStrMap::iterator obj_iter;

    try {
        msgpack::unpack(result, in_obj->object, in_obj->size);
        msgpack::object deserialized = result.get();
        dict = deserialized.as<MsgpackAdapter::MsgpackStrMap>();

        if ((obj_iter = dict.find("lat")) != dict.end()) {
            gpsinfo->lat = obj_iter->second.as<double>();
        }

        if ((obj_iter = dict.find("lon")) != dict.end()) {
            gpsinfo->lon = obj_iter->second.as<double>();
        }

        if ((obj_iter = dict.find("alt")) != dict.end()) {
            gpsinfo->alt = obj_iter->second.as<double>();
        }

        if ((obj_iter = dict.find("speed")) != dict.end()) {
            gpsinfo->speed = obj_iter->second.as<double>();
        }

        if ((obj_iter = dict.find("heading")) != dict.end()) {
            gpsinfo->heading = obj_iter->second.as<double>();
        }

        if ((obj_iter = dict.find("precision")) != dict.end()) {
            gpsinfo->precision = obj_iter->second.as<double>();
        }

        if ((obj_iter = dict.find("fix")) != dict.end()) {
            gpsinfo->precision = obj_iter->second.as<int32_t>();
        }

        if ((obj_iter = dict.find("time")) != dict.end()) {
            gpsinfo->time = (time_t) obj_iter->second.as<uint64_t>();
        }

        if ((obj_iter = dict.find("name")) != dict.end()) {
            gpsinfo->gpsname = obj_iter->second.as<string>();
        }

    } catch (const std::exception& e) {
        // Something went wrong with msgpack unpacking
        delete(gpsinfo);
        stringstream ss;
        ss << "failed to unpack gps bundle: " << e.what();

        trigger_error(ss.str());

        return NULL;
    }

    return gpsinfo;
}

kis_packet *KisDatasource::handle_kv_packet(KisDatasourceCapKeyedObject *in_obj) {
    // Extract a packet record
    
    kis_packet *packet = packetchain->GeneratePacket();
    kis_datachunk *datachunk = new kis_datachunk();

    // Unpack the dictionary
    MsgpackAdapter::MsgpackStrMap dict;
    msgpack::unpacked result;
    MsgpackAdapter::MsgpackStrMap::iterator obj_iter;

    try {
        msgpack::unpack(result, in_obj->object, in_obj->size);
        msgpack::object deserialized = result.get();
        dict = deserialized.as<MsgpackAdapter::MsgpackStrMap>();

        if ((obj_iter = dict.find("tv_sec")) != dict.end()) {
            packet->ts.tv_sec = (time_t) obj_iter->second.as<uint64_t>();
        } else {
            throw std::runtime_error(string("tv_sec timestamp missing"));
        }

        if ((obj_iter = dict.find("tv_usec")) != dict.end()) {
            packet->ts.tv_usec = (time_t) obj_iter->second.as<uint64_t>();
        } else {
            throw std::runtime_error(string("tv_usec timestamp missing"));
        }

        if ((obj_iter = dict.find("dlt")) != dict.end()) {
            datachunk->dlt = obj_iter->second.as<uint64_t>();
        } else {
            throw std::runtime_error(string("DLT missing"));
        }

        // Record the size
        uint64_t size = 0;
        if ((obj_iter = dict.find("size")) != dict.end()) {
            size = obj_iter->second.as<uint64_t>();
        } else {
            throw std::runtime_error(string("size field missing or zero"));
        }

        msgpack::object rawdata;
        if ((obj_iter = dict.find("packet")) != dict.end()) {
            rawdata = obj_iter->second;
        } else {
            throw std::runtime_error(string("packet data missing"));
        }

        if (rawdata.via.bin.size != size) {
            throw std::runtime_error(string("packet size did not match data size"));
        }

        datachunk->copy_data((const uint8_t *) rawdata.via.bin.ptr, size);

    } catch (const std::exception& e) {
        // Something went wrong with msgpack unpacking
        //
        // Destroy the packet appropriately
        packetchain->DestroyPacket(packet);
        // Always delete the datachunk, we don't insert it into the packet
        // until later
        delete(datachunk);

        stringstream ss;
        ss << "failed to unpack packet bundle: " << e.what();
        trigger_error(ss.str());

        return NULL;
    }

    packet->insert(pack_comp_linkframe, datachunk);

    return packet;
}

void KisDatasource::handle_kv_uuid(KisDatasourceCapKeyedObject *in_obj) {
    uuid parsed_uuid(string(in_obj->object, in_obj->size));

    if (parsed_uuid.error) {
        trigger_error("unable to parse UUID");
        return;
    }

    // Only set the local UUID if we don't define one in the sourceline
    if (!local_uuid) {
        set_source_uuid(parsed_uuid);
    }
}

void KisDatasource::handle_kv_config_channel(KisDatasourceCapKeyedObject *in_obj) {
    // Very simple - we just copy the channel string over
    set_int_source_channel(string(in_obj->object, in_obj->size));
}

void KisDatasource::handle_kv_config_hop(KisDatasourceCapKeyedObject *in_obj) {
    // Unpack the dictionary
    MsgpackAdapter::MsgpackStrMap dict;
    msgpack::unpacked result;
    MsgpackAdapter::MsgpackStrMap::iterator obj_iter;
    vector<string> channel_vec;

    try {
        msgpack::unpack(result, in_obj->object, in_obj->size);
        msgpack::object deserialized = result.get();
        dict = deserialized.as<MsgpackAdapter::MsgpackStrMap>();

        if ((obj_iter = dict.find("channels")) != dict.end()) {
            MsgpackAdapter::AsStringVector(obj_iter->second, channel_vec);

            // We now have a string vector of channels, dupe it into our 
            // tracked channels vec
            local_locker lock(&source_lock);

            TrackerElementVector hop_chan_vec(get_int_source_hop_vec());
            hop_chan_vec.clear();

            for (unsigned int x = 0; x < channel_vec.size(); x++) {
                SharedTrackerElement chanstr = 
                    channel_entry_builder->clone_type();
                chanstr->set(channel_vec[x]);
                hop_chan_vec.push_back(chanstr);
            }
        } else {
            throw std::runtime_error(string("channel list missing in hop config"));
        }

        if ((obj_iter = dict.find("rate")) != dict.end()) {
            set_int_source_hop_rate(obj_iter->second.as<double>());
        } else {
            throw std::runtime_error(string("rate missing in hop config"));
        }
    } catch (const std::exception& e) {
        // Something went wrong with msgpack unpacking
        stringstream ss;
        ss << "failed to unpack chanset bundle: " << e.what();
        trigger_error(ss.str());

        return;
    }
}

void KisDatasource::handle_kv_interfacelist(KisDatasourceCapKeyedObject *in_obj) {
    // Clears the list of interfaces, then extracts the array of new interfaces
    // from the packet
    
    listed_interfaces.clear();

    // Unpack the dictionary
    msgpack::unpacked result;
    MsgpackAdapter::MsgpackStrMap dict;
    MsgpackAdapter::MsgpackStrMap::iterator obj_iter;
    vector<string> channel_vec;

    try {
        msgpack::unpack(result, in_obj->object, in_obj->size);
        msgpack::object deserialized = result.get();

        // we expect an array of msgpack dicts, so turn it into an array
        for (unsigned int i = 0; i < deserialized.via.array.size; i++) {
            // Then turn it into a string map
            dict = deserialized.via.array.ptr[i].as<MsgpackAdapter::MsgpackStrMap>();

            // Our extracted values
            string interface;
            string opts;

            // Interface is mandatory, flags are not
            if ((obj_iter = dict.find("interface")) != dict.end()) {
                interface = obj_iter->second.as<string>();
            } else {
                throw std::runtime_error(string("interface missing in list response"));
            }

            if ((obj_iter = dict.find("flags")) != dict.end()) {
                opts = obj_iter->second.as<string>();
            }

            SharedInterface intf = static_pointer_cast<KisDatasourceInterface>(listed_interface_builder->clone_type());
            intf->populate(interface, opts);
            intf->set_prototype(get_source_builder());

            {
                local_locker lock(&source_lock);
                listed_interfaces.push_back(intf);
            }

        }
    } catch (const std::exception& e) {
        // Something went wrong with msgpack unpacking
        stringstream ss;
        ss << "failed to unpack proberesp channels bundle: " << e.what();

        trigger_error(ss.str());

        return;
    }

    return;
}

bool KisDatasource::write_packet(string in_cmd, KVmap in_kvpairs,
        uint32_t &ret_seqno) {
    // Generate a packet and put it in the buffer
    
    if (ringbuf_handler == NULL)
        return false;
    
    simple_cap_proto_t *ret = NULL;
    vector<simple_cap_proto_kv_t *> proto_kvpairs;
    size_t kvpair_len = 0;
    size_t kvpair_offt = 0;
    size_t pack_len;

    for (auto i = in_kvpairs.begin(); i != in_kvpairs.end(); ++i) {
        // Size of header + size of object
        simple_cap_proto_kv_t *kvt = (simple_cap_proto_kv_t *) 
            new char[sizeof(simple_cap_proto_kv_h_t) + i->second->size];

        // Set up the header, network endian
        snprintf(kvt->header.key, 16, "%s", i->second->key.c_str());
        kvt->header.obj_sz = kis_hton32(i->second->size);

        // Copy the content
        memcpy(kvt->object, i->second->object, i->second->size);

        // Add the total size
        kvpair_len += sizeof(simple_cap_proto_kv_h_t) + i->second->size;
    }

    // Make the container packet
    pack_len = sizeof(simple_cap_proto_t) + kvpair_len;

    ret = (simple_cap_proto_t *) new char[pack_len];

    ret->signature = kis_hton32(KIS_CAP_SIMPLE_PROTO_SIG);
   
    // Prep the checksum with 0
    ret->checksum = 0;

    {
        local_locker lock(&source_lock);
        ret->sequence_number = kis_hton32(next_cmd_sequence);
        ret_seqno = kis_ntoh32(next_cmd_sequence);
        next_cmd_sequence++;
    }

    ret->packet_sz = kis_hton32(pack_len);

    snprintf(ret->type, 16, "%s", in_cmd.c_str());

    ret->num_kv_pairs = kis_hton32(proto_kvpairs.size());

    // Progress through the kv pairs and pack them 
    for (unsigned int i = 0; i < proto_kvpairs.size(); i++) {
        // Annoying to have to do it this way
        size_t len = kis_ntoh32(proto_kvpairs[i]->header.obj_sz) +
            sizeof(simple_cap_proto_kv_h_t);

        memcpy(&(ret->data[kvpair_offt]), proto_kvpairs[i], len);

        kvpair_offt += len;

        // Delete it as we go
        delete(proto_kvpairs[i]);
    }

    // Calculate the checksum with it pre-populated as 0x0
    uint32_t calc_checksum;
    calc_checksum = Adler32Checksum((const char *) ret, pack_len);

    ret->checksum = kis_hton32(calc_checksum);

    size_t ret_sz;

    {
        // Lock & send to the ringbuffer
        local_locker lock(&source_lock);
        ret_sz = ringbuf_handler->PutWriteBufferData(ret, pack_len, true);
        
        // It's copied into the write buffer so delete our copy
        delete ret;
    }

    // Did we not fill the write buffer somehow?
    if (ret_sz != pack_len) {
        return false;
    }

    return true;
}

void KisDatasource::send_command_list_interfaces(unsigned int in_transaction,
        list_callback_t in_cb) {
    local_locker lock(&source_lock);

    KVmap kvmap;

    // Nothing to fill in for the kvmap for a list request

    uint32_t seqno;
    bool success;
    tracked_command *cmd;

    success = write_packet("LISTDEVICE", kvmap, seqno);

    if (!success) {
        if (in_cb != NULL) {
            in_cb(in_transaction, vector<SharedInterface>());
        }

        return;
    }

    cmd = new tracked_command(in_transaction, seqno, this);
    cmd->list_cb = in_cb;

    command_ack_map[seqno] = cmd;
}

void KisDatasource::send_command_probe_interface(string in_definition, 
        unsigned int in_transaction, probe_callback_t in_cb) {
    local_locker lock(&source_lock);

    KVmap kvmap;

    KisDatasourceCapKeyedObject *definition =
        new KisDatasourceCapKeyedObject("DEFINITION", in_definition.data(), 
                in_definition.length());
    kvmap.emplace("DEFINITION", definition);

    uint32_t seqno;
    bool success;
    tracked_command *cmd;

    success = write_packet("PROBEDEVICE", kvmap, seqno);

    if (!success) {
        if (in_cb != NULL) {
            in_cb(in_transaction, false, "unable to generate command frame");
        }

        return;
    }

    cmd = new tracked_command(in_transaction, seqno, this);
    cmd->probe_cb = in_cb;

    command_ack_map[seqno] = cmd;
}


void KisDatasource::send_command_open_interface(string in_definition,
        unsigned int in_transaction, open_callback_t in_cb) {
    local_locker lock(&source_lock);

    KisDatasourceCapKeyedObject *definition =
        new KisDatasourceCapKeyedObject("DEFINITION", in_definition.data(), 
                in_definition.length());

    KVmap kvmap;
    kvmap.emplace("DEFINITION", definition);

    uint32_t seqno;
    bool success;
    tracked_command *cmd;

    success = write_packet("OPENDEVICE", kvmap, seqno);

    if (!success) {
        if (in_cb != NULL) {
            in_cb(in_transaction, false, "unable to generate command frame");
        }

        return;
    }

    cmd = new tracked_command(in_transaction, seqno, this);
    cmd->open_cb = in_cb;

    command_ack_map[seqno] = cmd;
}

void KisDatasource::send_command_set_channel(string in_channel, 
        unsigned int in_transaction, configure_callback_t in_cb) {
    local_locker lock(&source_lock);

    KisDatasourceCapKeyedObject *chanset =
        new KisDatasourceCapKeyedObject("CHANSET", in_channel.data(),
                in_channel.length());
    KVmap kvmap;

    kvmap.emplace("CHANSET", chanset);

    uint32_t seqno;
    bool success;
    tracked_command *cmd;

    success = write_packet("CONFIGURE", kvmap, seqno);

    if (!success) {
        if (in_cb != NULL) {
            in_cb(in_transaction, false, "unable to generate command frame");
        }

        return;
    }

    cmd = new tracked_command(in_transaction, seqno, this);
    cmd->configure_cb = in_cb;

    command_ack_map[seqno] = cmd;
}

void KisDatasource::send_command_set_channel_hop(double in_rate, 
        SharedTrackerElement in_chans, unsigned int in_transaction,
        configure_callback_t in_cb) {

    // This is one of the more complex commands - we have to generate a 
    // command dictionary containing a rate:double and a channels:vector
    // structure; fortunately msgpack makes this easy for us.

    local_locker lock(&source_lock);

    TrackerElementVector in_vec(in_chans);

    // Pack the vector into a string stream using the msgpack api
    stringstream stream;
    msgpack::packer<std::stringstream> packer(&stream);

    // 2-element dictionary
    packer.pack_map(2);

    // Pack the rate dictionary entry
    packer.pack(string("rate"));
    packer.pack(in_rate);

    // Pack the vector of channels
    packer.pack(string("channels"));
    packer.pack_array(in_vec.size());

    for (auto i = in_vec.begin(); i != in_vec.end(); ++i) {
        packer.pack((*i)->get_string());
    }

    KisDatasourceCapKeyedObject *chanhop =
        new KisDatasourceCapKeyedObject("CHANHOP", stream.str().data(),
                stream.str().length());
    KVmap kvmap;

    kvmap.emplace("CHANHOP", chanhop);

    uint32_t seqno;
    bool success;
    tracked_command *cmd;

    success = write_packet("CONFIGURE", kvmap, seqno);

    if (!success) {
        if (in_cb != NULL) {
            in_cb(in_transaction, false, "unable to generate command frame");
        }

        return;
    }

    cmd = new tracked_command(in_transaction, seqno, this);
    cmd->configure_cb = in_cb;

    command_ack_map[seqno] = cmd;
}

void KisDatasource::register_fields() {
    tracker_component::register_fields();

    RegisterField("kismet.datasource.name", TrackerString,
            "Human-readable name", &source_name);
    RegisterField("kismet.datasource.uuid", TrackerUuid,
            "UUID", &source_uuid);

    RegisterField("kismet.datasource.definition", TrackerString,
            "Original source= definition", &source_definition);
    RegisterField("kismet.datasource.interface", TrackerString,
            "Interface", &source_interface);

    channel_entry_builder.reset(new TrackerElement(TrackerString, 0));
    RegisterComplexField("kismet.datasource.channel_entry",
            channel_entry_builder, "Channel");

    RegisterField("kismet.datasource.channels", TrackerVector,
            "Supported channels", &source_channels_vec);
    RegisterField("kismet.datasource.hopping", TrackerUInt8,
            "Source is channel hopping", &source_hopping);
    RegisterField("kismet.datasource.channel", TrackerString,
            "Current channel", &source_channel);
    RegisterField("kismet.datasource.hop_rate", TrackerDouble,
            "Hop rate if channel hopping", &source_hop_rate);
    RegisterField("kismet.datasource.hop_channels", TrackerVector,
            "Hop pattern if hopping", &source_hop_vec);

    RegisterField("kismet.datasource.error", TrackerUInt8,
            "Source is in error state", &source_error);
    RegisterField("kismet.datasource.error_reason", TrackerString,
            "Last known reason for error state", &source_error_reason);

    RegisterField("kismet.datasource.retry", TrackerUInt8,
            "Source will try to re-open after failure", &source_retry);
    RegisterField("kismet.datasource.retry_attempts", TrackerUInt32,
            "Consecutive unsuccessful retry attempts", &source_retry_attempts);

}

void KisDatasource::reserve_fields(SharedTrackerElement e) {
    tracker_component::reserve_fields(e);

    // We don't ever instantiate from an existing object so we don't do anything
}

void KisDatasource::handle_source_error() {
    local_locker lock(&source_lock);

    stringstream ss;

    // Do nothing if we don't handle retry
    if (!get_source_retry()) {
        ss << "Source " << get_source_name() << " has encountered an error but "
            "is not configured to automatically re-try opening; it will remain "
            "closed.";
        _MSG(ss.str(), MSGFLAG_ERROR);

        return;
    }

    // Increment our failures
    inc_int_source_retry_attempts(1);

    // Notify about it
    ss << "Source " << get_source_name() << " has encountered an error. "
        "Kismet will attempt to re-open the source in 5 seconds.  (" <<
        get_source_retry_attempts() << " failures)";
    _MSG(ss.str(), MSGFLAG_ERROR);

    // Cancel any timers
    if (error_timer_id > 0)
        timetracker->RemoveTimer(error_timer_id);

    // Set a new event to try to re-open the interface
    error_timer_id = timetracker->RegisterTimer(SERVER_TIMESLICES_SEC * 5,
            NULL, 0, [this](int) -> int {
                // Call open on the same sourceline, no transaction, no cb
                open_interface(get_source_definition(), 0, NULL);
                return 0;
            });
}


KisDatasourceCapKeyedObject::KisDatasourceCapKeyedObject(simple_cap_proto_kv *in_kp) {
    char ckey[17];

    snprintf(ckey, 17, "%s", in_kp->header.key);
    key = string(ckey);

    size = kis_ntoh32(in_kp->header.obj_sz);
    object = new char[size];
    memcpy(object, in_kp->object, size);
}

KisDatasourceCapKeyedObject::KisDatasourceCapKeyedObject(string in_key,
        const char *in_object, ssize_t in_len) {

    key = in_key.substr(0, 16);
    object = new char[in_len];
    memcpy(object, in_object, in_len);
}

KisDatasourceCapKeyedObject::~KisDatasourceCapKeyedObject() {
    delete[] object;
}

#if 0

void KisDataSource::BufferAvailable(size_t in_amt) {
    simple_cap_proto_t *frame_header;
    uint8_t *buf;
    uint32_t frame_sz;
    uint32_t frame_checksum, calc_checksum;

    if (in_amt < sizeof(simple_cap_proto_t)) {
        return;
    }

    // Peek the buffer
    buf = new uint8_t[in_amt];
    ringbuf_handler->PeekReadBufferData(buf, in_amt);

    frame_header = (simple_cap_proto_t *) buf;

    if (kis_ntoh32(frame_header->signature) != KIS_CAP_SIMPLE_PROTO_SIG) {
        _MSG("Kismet data source " + get_source_name() + " got an invalid "
                "control from on IPC/Network, closing.", MSGFLAG_ERROR);
        delete[] buf;
        close_source();
        return;
    }

    frame_sz = kis_ntoh32(frame_header->packet_sz);

    if (frame_sz > in_amt) {
        // Nothing we can do right now, not enough data to make up a complete packet.
        delete[] buf;
        return;
    }

    // Get the checksum & save it
    frame_checksum = kis_ntoh32(frame_header->checksum);

    // Zero the checksum field in the packet
    frame_header->checksum = 0x00000000;

    // Calc the checksum of the rest
    calc_checksum = Adler32Checksum((const char *) buf, frame_sz);

    // Compare to the saved checksum
    if (calc_checksum != frame_checksum) {
        // TODO report invalid checksum and disconnect
        delete[] buf;
        return;
    }

    // Consume the packet in the ringbuf 
    ringbuf_handler->GetReadBufferData(NULL, frame_sz);

    // Extract the kv pairs
    KVmap kv_map;

    ssize_t data_offt = 0;
    for (unsigned int kvn = 0; kvn < kis_ntoh32(frame_header->num_kv_pairs); kvn++) {
        simple_cap_proto_kv *pkv =
            (simple_cap_proto_kv *) &((frame_header->data)[data_offt]);

        data_offt = 
            sizeof(simple_cap_proto_kv_h_t) +
            kis_ntoh32(pkv->header.obj_sz);

        KisDataSource_CapKeyedObject *kv =
            new KisDataSource_CapKeyedObject(pkv);

        kv_map[StrLower(kv->key)] = kv;
    }

    char ctype[17];
    snprintf(ctype, 17, "%s", frame_header->type);
    handle_packet(ctype, kv_map);

    for (KVmap::iterator i = kv_map.begin(); i != kv_map.end(); ++i) {
        delete i->second;
    }

    delete[] buf;

}

void KisDataSource::BufferError(string in_error) {
    _MSG(in_error, MSGFLAG_ERROR);
    
    {
        local_locker lock(&source_lock);

        if (probe_cb != NULL) {
            probe_cb(false, probe_transaction);
        }

        if (list_cb != NULL) {
            list_cb(vector<SharedListInterface>());
        }

        if (ipc_remote != NULL) {
            // Kill the IPC
            ipc_remote->soft_kill();

            set_source_running(false);
            set_child_pid(-1);
        } else if (ringbuf_handler != NULL) {
            datasourcetracker->KillConnection(ringbuf_handler);
        }

    }
}

bool KisDataSource::write_packet(string in_cmd, KVmap in_kvmap) {
}


int KisDataSource::probe_source(string in_source, unsigned int in_transaction,
        function<void (bool, unsigned int)> in_cb) {
    local_locker lock(&source_lock);

    /* Inherited functions must fill this in.
     *
     * Non-ipc probing should be handled immediately, ipc probe should
     * launch ipc and queue a probe command, returning the results to the
     * callback when the probe command completes
     */

    probe_cb = in_cb;
    probe_transaction = in_transaction;

    set_sourceline(in_source);

    return 0;
}

int KisDataSource::open_local_source(string in_source, unsigned int in_transaction,
        function<void (bool, unsigned int)> in_cb) {
    local_locker lock(&source_lock);

    /* Inherited functions must fill this in.
     *
     * Non-IPC sources can perform an open directly
     * IPC sources should use src_send_open
     *
     */

    open_cb = in_cb;
    open_transaction = in_transaction;
    set_sourceline(in_source);

    return 0;
}

bool KisDataSource::open_network_source(shared_ptr<RingbufferHandler> in_handler) {
    local_locker lock(&source_lock);

    /* TODO figure out semantics of inheriting name, channels, etc from
     * remote source */

    ringbuf_handler = in_handler;

    return true;
}

bool KisDataSource::open_passive_source(string srcdef) {
    local_locker lock(&source_lock);

    /* TODO figure out semantics of this */

    set_sourceline(srcdef);

    return true;
}

vector<SharedListInterface> KisDataSource::list_interfaces(unsigned int in_transaction,
        function<void (unsigned int, vector<SharedListInterface>)> in_cb) {
    local_locker lock(&source_lock);

    list_cb = in_cb;
    list_transaction = in_transaction;

    /* TODO figure out this, too */

    return vector<SharedListInterface>();
}


bool KisDataSource::src_set_source_hop_vec(SharedTrackerElement in_vec) {
    return set_channel_hop(in_vec, get_source_hop_rate());
}

bool KisDataSource::src_set_source_hop_rate(double in_rate) {
    return set_channel_hop(get_source_hop_vec(), in_rate);
}

void KisDataSource::handle_packet(string in_type, KVmap in_kvmap) {
    string ltype = StrLower(in_type);

    if (ltype == "status")
        handle_packet_status(in_kvmap);
    else if (ltype == "proberesp")
        handle_packet_probe_resp(in_kvmap);
    else if (ltype == "openresp")
        handle_packet_open_resp(in_kvmap);
    else if (ltype == "error")
        handle_packet_error(in_kvmap);
    else if (ltype == "message")
        handle_packet_message(in_kvmap);
    else if (ltype == "data")
        handle_packet_data(in_kvmap);
}

void KisDataSource::handle_packet_status(KVmap in_kvpairs) {
    KVmap::iterator i;
    
    if ((i = in_kvpairs.find("message")) != in_kvpairs.end()) {
        handle_kv_message(i->second);
    }

}

void KisDataSource::handle_packet_probe_resp(KVmap in_kvpairs) {
    KVmap::iterator i;

    // Process any messages
    if ((i = in_kvpairs.find("message")) != in_kvpairs.end()) {
        handle_kv_message(i->second);
    }

    // Process channels list if we got one
    if ((i = in_kvpairs.find("channels")) != in_kvpairs.end()) {
        if (!handle_kv_channels(i->second))
            return;
    }

    // Process success value and callback
    if ((i = in_kvpairs.find("success")) != in_kvpairs.end()) {
        local_locker lock(&source_lock);

        if (probe_cb != NULL) {
            probe_cb(handle_kv_success(i->second), probe_transaction);
        }

    } else {
        // ProbeResp with no success value?  ehh.
        BufferError("Invalid interface probe response");
        return;
    }

    if (ipc_remote != NULL)
        ipc_remote->close_ipc();
}

void KisDataSource::handle_packet_open_resp(KVmap in_kvpairs) {
    KVmap::iterator i;

    // Process any messages
    if ((i = in_kvpairs.find("message")) != in_kvpairs.end()) {
        handle_kv_message(i->second);
    }

    // Process channels list if we got one
    if ((i = in_kvpairs.find("channels")) != in_kvpairs.end()) {
        if (!handle_kv_channels(i->second))
            return;
    }

    // Process success value and callback
    if ((i = in_kvpairs.find("success")) != in_kvpairs.end()) {
        local_locker lock(&source_lock);

        if (open_cb != NULL) {
            open_cb(handle_kv_success(i->second), open_transaction);
        }
    } else {
        // OpenResp with no success value?  ehh.
        BufferError("Invalid interface open response");
        return;
    }
}

void KisDataSource::handle_packet_error(KVmap in_kvpairs) {
    KVmap::iterator i;

    // Process any messages
    if ((i = in_kvpairs.find("message")) != in_kvpairs.end()) {
        handle_kv_message(i->second);
    }

    // Lock only after handling messages
    {
        local_locker lock(&source_lock);

        if (ipc_remote != NULL) {
            ipc_remote->soft_kill();

            set_source_running(false);
            set_child_pid(0);
        } else if (ringbuf_handler != NULL) {
            datasourcetracker->KillConnection(ringbuf_handler);
        }
    }
}


void KisDataSource::handle_packet_message(KVmap in_kvpairs) {
    KVmap::iterator i;

    // Process any messages
    if ((i = in_kvpairs.find("message")) != in_kvpairs.end()) {
        handle_kv_message(i->second);
    }
}

void KisDataSource::handle_packet_data(KVmap in_kvpairs) {
    KVmap::iterator i;

    kis_packet *packet = NULL;
    kis_layer1_packinfo *siginfo = NULL;
    kis_gps_packinfo *gpsinfo = NULL;

    // Process any messages
    if ((i = in_kvpairs.find("message")) != in_kvpairs.end()) {
        handle_kv_message(i->second);
    }

    // Do we have a packet?
    if ((i = in_kvpairs.find("packet")) != in_kvpairs.end()) {
        packet = handle_kv_packet(i->second);
    }

    if (packet == NULL)
        return;

    // Gather signal data
    if ((i = in_kvpairs.find("signal")) != in_kvpairs.end()) {
        siginfo = handle_kv_signal(i->second);
    }
    
    // Gather GPS data
    if ((i = in_kvpairs.find("gps")) != in_kvpairs.end()) {
        gpsinfo = handle_kv_gps(i->second);
    }

    // Add them to the packet
    if (siginfo != NULL) {
        packet->insert(pack_comp_l1info, siginfo);
    }

    if (gpsinfo != NULL) {
        packet->insert(pack_comp_gps, gpsinfo);
    }

    // Inject the packet into the packetchain if we have one
    packetchain->ProcessPacket(packet);

}


bool KisDataSource::spawn_ipc() {
    stringstream ss;

    // Do not lock thread, we can only be called when we're inside a locked
    // context.
    
    set_source_running(false);
    set_child_pid(0);

    if (get_source_ipc_bin() == "") {
        ss << "Datasource '" << get_source_name() << "' missing IPC binary, cannot "
            "launch binary";
        
        _MSG(ss.str(), MSGFLAG_ERROR);

        return false;
    }

    // Kill the running process if we have one
    if (ipc_remote != NULL) {
        ss.str("");
        ss << "Datasource '" << get_source_name() << "' launching IPC with a running "
            "process, killing existing process pid " << get_child_pid();
        _MSG(ss.str(), MSGFLAG_INFO);

        ipc_remote->soft_kill();
    }

    // Make a new handler and new ipc.  Give a generous buffer.
    ringbuf_handler.reset(new RingbufferHandler((32 * 1024), (32 * 1024)));
    ringbuf_handler->SetReadBufferInterface(this);

    ipc_remote = new IPCRemoteV2(globalreg, ringbuf_handler);

    // Get allowed paths for binaries
    vector<string> bin_paths = globalreg->kismet_config->FetchOptVec("bin_paths");

    for (vector<string>::iterator i = bin_paths.begin(); i != bin_paths.end(); ++i) {
        ipc_remote->add_path(*i);
    }

    vector<string> args;

    int ret = ipc_remote->launch_kis_binary(get_source_ipc_bin(), args);

    if (ret < 0) {
        ss.str("");
        ss << "Datasource '" << get_source_name() << "' failed to launch IPC " <<
            "binary '" << get_source_ipc_bin() << "'";
        _MSG(ss.str(), MSGFLAG_ERROR);

        return false;
    }

    set_source_running(true);
    set_child_pid(ipc_remote->get_pid());

    return true;
}

KisDataSource_CapKeyedObject::KisDataSource_CapKeyedObject(simple_cap_proto_kv *in_kp) {
    char ckey[17];

    snprintf(ckey, 17, "%s", in_kp->header.key);
    key = string(ckey);

    size = kis_ntoh32(in_kp->header.obj_sz);
    object = new char[size];
    memcpy(object, in_kp->object, size);
}

KisDataSource_CapKeyedObject::KisDataSource_CapKeyedObject(string in_key,
        const char *in_object, ssize_t in_len) {

    key = in_key.substr(0, 16);
    object = new char[in_len];
    memcpy(object, in_object, in_len);
}

KisDataSource_CapKeyedObject::~KisDataSource_CapKeyedObject() {
    delete[] object;
}
#endif

