/*
 * JILL - C++ framework for JACK
 *
 * includes code from klick, Copyright (C) 2007-2009  Dominic Sacre  <dominic.sacre@gmx.de>
 * additions Copyright (C) 2010 C Daniel Meliza <dmeliza@uchicago.edu>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef _OFFLINE_APPLICATION_HH
#define _OFFLINE_APPLICATION_HH

#include "application.hh"
#include "audio_interface_offline.hh"

namespace jill {

/**
 * The OfflineApplication is for simulating a JACK run. Rather than use
 * the JACK server to run the process thread, it reads in an input
 * file and passes the data to the client's process loop.  Both the
 * main loop and process loop are run in the same thread.
 */
class OfflineApplication : public Application {

public:
	OfflineApplication(AudioInterfaceOffline &client, util::logstream &logv);
	virtual ~OfflineApplication() {}

	virtual void set_mainloop_callback(const MainLoopCallback &cb) { _mainloop_cb = cb; } 

        /**
	 * Process data in the input file. Data are passed to the
	 * client's process function in blocks; the main loop callback
	 * is run after every @a nblocks runs of the process function.
	 *
	 * @param nblocks The number of blocks to process between
	 * calls to main loop.
	 */
	virtual void run(unsigned int nblocks=2);

	/// Terminate the application at the end of the next main loop; no-op in this class
	virtual void signal_quit() {}

protected:
	/// A stream for producing log messages.
	util::logstream &_logv;

private:

	AudioInterfaceOffline &_client;
};

/**
 * This class should be used instead of JillOptions when running an offline test
 */
class OfflineOptions : public Options {
public:
	OfflineOptions(const char *program_name, const char *program_version);
	virtual ~OfflineOptions() {}

	/// The input file name
	std::string input_file;
	/// The output file name
	std::string output_file;
	/// The log file to write application events to
	std::string logfile;
	/// The size of block to read from the input file or write to output file
	int blocksize;
	/// The sampling rate to use in opening up output file, if no input file supplied
	int samplerate;

protected:
	virtual void process_options();

};

}

#endif