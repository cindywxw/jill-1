/* @file arf_sndfile.cc
 *
 * Copyright (C) 2011 C Daniel Meliza <dan@meliza.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "arf_thread.hh"
#include <boost/format.hpp>
#include <sys/time.h>
#include <arf.hpp>
#include "../jack_client.hh"
#include "../midi.hh"
#include "../util/string.hh"
#include "../dsp/period_ringbuffer.hh"

using namespace std;

// template specializations for compound data types
namespace arf { namespace h5t { namespace detail {

template<>
struct datatype_traits<jill::file::message_t> {
	static hid_t value() {
                hid_t str = H5Tcopy(H5T_C_S1);
                H5Tset_size(str, H5T_VARIABLE);
                hid_t ret = H5Tcreate(H5T_COMPOUND, sizeof(jill::file::message_t));
                H5Tinsert(ret, "sec", HOFFSET(jill::file::message_t, sec), H5T_STD_I64LE);
                H5Tinsert(ret, "usec", HOFFSET(jill::file::message_t, usec), H5T_STD_I64LE);
                H5Tinsert(ret, "message", HOFFSET(jill::file::message_t, message), str);
                H5Tclose(str);
                return ret;
        }
};

template<>
struct datatype_traits<jill::file::event_t> {
	static hid_t value() {
                hid_t str = H5Tcopy(H5T_C_S1);
                H5Tset_size(str, H5T_VARIABLE);
                hid_t ret = H5Tcreate(H5T_COMPOUND, sizeof(jill::file::event_t));
                H5Tinsert(ret, "start", HOFFSET(jill::file::event_t, start), H5T_STD_U32LE);
                H5Tinsert(ret, "type", HOFFSET(jill::file::event_t, type), H5T_NATIVE_CHAR);
                H5Tinsert(ret, "chan", HOFFSET(jill::file::event_t, chan), H5T_NATIVE_CHAR);
                H5Tinsert(ret, "message", HOFFSET(jill::file::event_t, message), str);
                H5Tclose(str);
                return ret;
        }
};

}}}

using namespace jill::file;
using jill::util::make_string;

pthread_mutex_t disk_thread_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  data_ready = PTHREAD_COND_INITIALIZER;

arf_thread::arf_thread(string const & filename,
                       map<string,string> const * attrs,
                       jack_client * client,
                       dsp::period_ringbuffer * ringbuf,
                       int compression)
        : _client(client), _ringbuf(ringbuf),
          _attrs(attrs),
          _xruns(0), _stop(0), _compression(compression)
{
        open_arf(filename);
}

arf_thread::~arf_thread()
{
        // prevent access to file during destruction
        pthread_mutex_lock (&disk_thread_lock);
        _dsets.clear();
        _entry.reset();
        _file.reset();
        // TODO unflock the file
	pthread_mutex_unlock (&disk_thread_lock);
}

void
arf_thread::new_entry(nframes_t frame, timeval const * timestamp)
{

        int idx = _file->nchildren();
        boost::format fmt("entry_%|06|");
        fmt % idx;

        _entry.reset(new arf::entry(*_file, fmt.str(), timestamp));
        _entry->write_attribute()("jack_frame",frame)("jack_usec",_client->time(frame));
        if (_attrs) {
                for_each(_attrs->begin(), _attrs->end(), _entry->write_attribute());
        }

}

void
arf_thread::new_datasets()
{
        _dsets.clear();
        list<jack_port_t*>::const_iterator it;
        for (it = _client->ports().begin(); it != _client->ports().end(); ++it) {
                arf::packet_table_ptr pt;
                char const * name = jack_port_short_name(*it);
                if (strcmp(jack_port_type(*it),JACK_DEFAULT_AUDIO_TYPE)==0) {
                        pt = _entry->create_packet_table<sample_t>(name,"", arf::UNDEFINED,
                                                                   false, 1024, _compression);
                }
                else {
                        pt = _entry->create_packet_table<jill::file::event_t>(name,"samples",arf::EVENT,
                                                                              false, 1024, _compression);
                }
                // times are stored in units of samples for maximum precision,
                // which requires sample rates to be known.
                pt->write_attribute("sampling_rate", _client->sampling_rate());
                _dsets.push_back(pt);
        }
}

void
arf_thread::open_arf(string const & filename)
{
        // TODO flock the file if on a local disk
        _file.reset(new arf::file(filename, "a"));

        // open/create log
        if (_file->contains(JILL_LOGDATASET_NAME)) {
                _log.reset(new arf::h5pt::packet_table(_file->hid(), JILL_LOGDATASET_NAME));
                // compare datatype
                arf::h5t::wrapper<jill::file::message_t> t;
                arf::h5t::datatype expected(t);
                if (expected != *(_log->datatype())) {
                        throw arf::Exception(JILL_LOGDATASET_NAME " has wrong datatype");
                }
        }
        else {
                _log = _file->create_packet_table<jill::file::message_t>(JILL_LOGDATASET_NAME);
        }

}

void
arf_thread::log(string const & msg, boost::int64_t sec, boost::int64_t usec)
{
        // log may be called by a jack callback and needs to lock mutex
        pthread_mutex_lock (&disk_thread_lock);
        if (_file && _log) {
                jill::file::message_t message = { sec, usec, msg.c_str() };
                _log->write(&message, 1);
        }
	pthread_mutex_unlock (&disk_thread_lock);
}

void
arf_thread::log(string const & msg)
{
        struct timeval tp;
        gettimeofday(&tp,0);
        log(msg, tp.tv_sec, tp.tv_usec);
}


void
arf_thread::start()
{
        int ret = pthread_create(&_thread_id, NULL, arf_thread::write_continuous, this);
        if (ret != 0)
                throw std::runtime_error("Failed to start disk thread");
}

void
arf_thread::stop()
{
        _stop = 1;
}


void
arf_thread::join()
{
        pthread_join(_thread_id, NULL);
}

/*
 * Write data to arf file in continous mode.
 *
 * @pre client is started and ports registered
 * @pre outfile is initialized
 */
void *
arf_thread::write_continuous(void * arg)
{
        arf_thread * self = static_cast<arf_thread *>(arg);
        dsp::period_ringbuffer::period_info_t const * period;
        long my_xruns = 0;                       // internal counter
        timeval tp;
        nframes_t entry_start;

	pthread_setcanceltype (PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
	pthread_mutex_lock (&disk_thread_lock);

        while (1) {
                period = self->_ringbuf->request();
                if (period) {
                        // cout << "got period" << endl;
                        assert(period->nchannels == self->_client->nports());
                        // create entry when first data chunk arrives. Also
                        // break entries if the counter overflows to ensure that
                        // sample-based time values within the entry are
                        // consistent. This may happen early for the first
                        // entry, but the math is too hard otherwise...
                        if (!self->_entry || period->time < entry_start) {
                                gettimeofday(&tp,0);
                                entry_start = period->time;
                                self->new_entry(period->time, &tp);
                                self->new_datasets();
                        }

                        // write channels
                        size_t i = 0;
                        list<jack_port_t*>::const_iterator p = self->_client->ports().begin();
                        for (; i < period->nchannels; ++i, ++p) {
                                void * data = self->_ringbuf->peek(i);
                                // look up channel name and type
                                if (strcmp(jack_port_type(*p),JACK_DEFAULT_AUDIO_TYPE)==0) {
                                        self->_dsets[i]->write(data, period->nbytes / sizeof(sample_t));
                                }
                                else {
                                        jack_midi_event_t event;
                                        nframes_t adj_time;
                                        nframes_t nevents = jack_midi_get_event_count(data);
                                        for (nframes_t j = 0; j < nevents; ++j) {
                                                jack_midi_event_get(&event, data, j);
                                                adj_time = event.time + period->time - entry_start;
                                                // TODO package in event_t
                                        }
                                }
                        }
                        // free data
                        self->_ringbuf->pop_all(0);
                        // check for overrun
                        if (my_xruns < self->_xruns) {
                                // TODO mark entry; start new entry
                        }
                }
                else if (self->_stop) {
                        goto done;
                }
                else {
                        // wait on data_ready
                        // cout << "waiting" << endl;
                        pthread_cond_wait (&data_ready, &disk_thread_lock);
                }
        }

done:
        self->_file->flush();
	pthread_mutex_unlock (&disk_thread_lock);
        return 0;
}
