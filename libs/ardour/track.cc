/*
    Copyright (C) 2006 Paul Davis

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/
#include "pbd/error.h"

#include "ardour/amp.h"
#include "ardour/audioengine.h"
#include "ardour/audiofilesource.h"
#include "ardour/audioregion.h"
#include "ardour/debug.h"
#include "ardour/delivery.h"
#include "ardour/disk_reader.h"
#include "ardour/disk_writer.h"
#include "ardour/event_type_map.h"
#include "ardour/io_processor.h"
#include "ardour/meter.h"
#include "ardour/midi_region.h"
#include "ardour/monitor_control.h"
#include "ardour/playlist.h"
#include "ardour/playlist_factory.h"
#include "ardour/port.h"
#include "ardour/processor.h"
#include "ardour/profile.h"
#include "ardour/region_factory.h"
#include "ardour/record_enable_control.h"
#include "ardour/record_safe_control.h"
#include "ardour/route_group_specialized.h"
#include "ardour/session.h"
#include "ardour/session_playlists.h"
#include "ardour/smf_source.h"
#include "ardour/track.h"
#include "ardour/types_convert.h"
#include "ardour/utils.h"

#include "pbd/i18n.h"

using namespace std;
using namespace ARDOUR;
using namespace PBD;

Track::Track (Session& sess, string name, PresentationInfo::Flag flag, TrackMode mode, DataType default_type)
	: Route (sess, name, flag, default_type)
        , _saved_meter_point (_meter_point)
        , _mode (mode)
	, _alignment_choice (Automatic)
{
	_freeze_record.state = NoFreeze;
        _declickable = true;

}

Track::~Track ()
{
	DEBUG_TRACE (DEBUG::Destruction, string_compose ("track %1 destructor\n", _name));

	if (_disk_reader) {
		_disk_reader->set_route (boost::shared_ptr<Route>());
		_disk_reader.reset ();
	}

	if (_disk_writer) {
		_disk_writer->set_route (boost::shared_ptr<Route>());
		_disk_writer.reset ();
	}
}

int
Track::init ()
{
        if (Route::init ()) {
                return -1;
        }

        DiskIOProcessor::Flag dflags = DiskIOProcessor::Recordable;

        if (_mode == Destructive && !Profile->get_trx()) {
	        dflags = DiskIOProcessor::Flag (dflags | DiskIOProcessor::Destructive);
        } else if (_mode == NonLayered){
	        dflags = DiskIOProcessor::Flag(dflags | DiskIOProcessor::NonLayered);
        }

        _disk_reader.reset (new DiskReader (_session, name(), dflags));
        _disk_reader->set_block_size (_session.get_block_size ());
        _disk_reader->set_route (boost::dynamic_pointer_cast<Route> (shared_from_this()));

        _disk_writer.reset (new DiskWriter (_session, name(), dflags));
        _disk_writer->set_block_size (_session.get_block_size ());
        _disk_writer->set_route (boost::dynamic_pointer_cast<Route> (shared_from_this()));

        use_new_playlist (data_type());

        boost::shared_ptr<Route> rp (boost::dynamic_pointer_cast<Route> (shared_from_this()));
	boost::shared_ptr<Track> rt = boost::dynamic_pointer_cast<Track> (rp);

	_record_enable_control.reset (new RecordEnableControl (_session, EventTypeMap::instance().to_symbol (RecEnableAutomation), *this));
	add_control (_record_enable_control);

	_record_safe_control.reset (new RecordSafeControl (_session, EventTypeMap::instance().to_symbol (RecSafeAutomation), *this));
	add_control (_record_safe_control);

	_monitoring_control.reset (new MonitorControl (_session, EventTypeMap::instance().to_symbol (MonitoringAutomation), *this));
	add_control (_monitoring_control);

	_session.config.ParameterChanged.connect_same_thread (*this, boost::bind (&Track::parameter_changed, this, _1));

        _monitoring_control->Changed.connect_same_thread (*this, boost::bind (&Track::monitoring_changed, this, _1, _2));
        _record_safe_control->Changed.connect_same_thread (*this, boost::bind (&Track::record_safe_changed, this, _1, _2));
        _record_enable_control->Changed.connect_same_thread (*this, boost::bind (&Track::record_enable_changed, this, _1, _2));

        _input->changed.connect_same_thread (*this, boost::bind (&Track::input_changed, this));

        return 0;
}

void
Track::input_changed ()
{
	if (_disk_writer && _alignment_choice == Automatic) {
		set_align_choice_from_io ();
	}
}

XMLNode&
Track::get_state ()
{
	return state (true);
}

XMLNode&
Track::state (bool full)
{
	XMLNode& root (Route::state (full));

	if (_playlists[DataType::AUDIO]) {
		root.set_property (X_("audio-playlist"), _playlists[DataType::AUDIO]->id().to_s());
	}

	if (_playlists[DataType::MIDI]) {
		root.set_property (X_("midi-playlist"), _playlists[DataType::MIDI]->id().to_s());
	}

	root.add_child_nocopy (_monitoring_control->get_state ());
	root.add_child_nocopy (_record_safe_control->get_state ());
	root.add_child_nocopy (_record_enable_control->get_state ());

	root.set_property (X_("saved-meter-point"), _saved_meter_point);
	root.set_property (X_("alignment-choice"), _alignment_choice);

	return root;
}

int
Track::set_state (const XMLNode& node, int version)
{
	if (Route::set_state (node, version)) {
		return -1;
	}

	XMLNode* child;

	if (version >= 3000 && version < 4000) {
		if ((child = find_named_node (node, X_("Diskstream"))) != 0) {
			/* XXX if we remember anything from stored DiskStream
			   state (older Ardour versions) that is needed by a
			   DiskReader or DiskWriter, we should cook up a new
			   XMLNode here, populate it with that information
			   (child nodes, properties, etc.) and then call
			   ::set_state() on the writer/reader.

			   But at present (June 2017), there's no such state.
			*/
		}
	}

	std::string playlist_id;

	if (node.get_property (X_("audio-playlist"), playlist_id)) {
		find_and_use_playlist (DataType::AUDIO, PBD::ID (playlist_id));
	}

	if (node.get_property (X_("midi-playlist"), playlist_id)) {
		find_and_use_playlist (DataType::MIDI, PBD::ID (playlist_id));
	}

	XMLNodeList nlist = node.children();
	for (XMLNodeConstIterator niter = nlist.begin(); niter != nlist.end(); ++niter) {
		child = *niter;

		if (child->name() == Controllable::xml_node_name) {
			std::string name;
			if (!child->get_property ("name", name)) {
				continue;
			}

			if (name == _record_enable_control->name()) {
				_record_enable_control->set_state (*child, version);
			} else if (name == _record_safe_control->name()) {
				_record_safe_control->set_state (*child, version);
			} else if (name == _monitoring_control->name()) {
				_monitoring_control->set_state (*child, version);
			}
		}
	}

	if (!node.get_property (X_("saved-meter-point"), _saved_meter_point)) {
		_saved_meter_point = _meter_point;
	}


	AlignChoice ac;

	if (node.get_property (X_("alignment-choice"), ac)) {
		set_align_choice (ac, true);
	}

	return 0;
}

XMLNode&
Track::get_template ()
{
	return state (false);
}

Track::FreezeRecord::~FreezeRecord ()
{
	for (vector<FreezeRecordProcessorInfo*>::iterator i = processor_info.begin(); i != processor_info.end(); ++i) {
		delete *i;
	}
}

Track::FreezeState
Track::freeze_state() const
{
	return _freeze_record.state;
}

bool
Track::can_record()
{
	bool will_record = true;
	for (PortSet::iterator i = _input->ports().begin(); i != _input->ports().end() && will_record; ++i) {
		if (!i->connected())
			will_record = false;
	}

	return will_record;
}

int
Track::prep_record_enabled (bool yn)
{
	if (yn && _record_safe_control->get_value()) {
		return -1;
	}

	if (!can_be_record_enabled()) {
		return -1;
	}

	/* keep track of the meter point as it was before we rec-enabled */
	if (!_disk_writer->record_enabled()) {
		_saved_meter_point = _meter_point;
	}

	bool will_follow;

	if (yn) {
		will_follow = _disk_writer->prep_record_enable ();
	} else {
		will_follow = _disk_writer->prep_record_disable ();
	}

	if (will_follow) {
		if (yn) {
			if (_meter_point != MeterCustom) {
				set_meter_point (MeterInput);
			}
		} else {
			set_meter_point (_saved_meter_point);
		}
	}

	return 0;
}

void
Track::record_enable_changed (bool, Controllable::GroupControlDisposition)
{
	_disk_writer->set_record_enabled (_record_enable_control->get_value());
}

void
Track::record_safe_changed (bool, Controllable::GroupControlDisposition)
{
	_disk_writer->set_record_safe (_record_safe_control->get_value());
}

bool
Track::can_be_record_safe ()
{
	return !_record_enable_control->get_value() && _disk_writer && _session.writable() && (_freeze_record.state != Frozen);
}

bool
Track::can_be_record_enabled ()
{
	return !_record_safe_control->get_value() && _disk_writer && !_disk_writer->record_safe() && _session.writable() && (_freeze_record.state != Frozen);
}

void
Track::parameter_changed (string const & p)
{
	if (p == "track-name-number") {
		resync_track_name ();
	}
	else if (p == "track-name-take") {
		resync_track_name ();
	}
	else if (p == "take-name") {
		if (_session.config.get_track_name_take()) {
			resync_track_name ();
		}
	}
}

void
Track::resync_track_name ()
{
	set_name(name());
}

bool
Track::set_name (const string& str)
{
	bool ret;

	if (str.empty ()) {
		return false;
	}

	if (_record_enable_control->get_value()) {
		/* when re-arm'ed the file (named after the track) is already ready to rolll */
		return false;
	}

	string diskstream_name = "";
	if (_session.config.get_track_name_take () && !_session.config.get_take_name ().empty()) {
		// Note: any text is fine, legalize_for_path() fixes this later
		diskstream_name += _session.config.get_take_name ();
		diskstream_name += "_";
	}
	const int64_t tracknumber = track_number();
	if (tracknumber > 0 && _session.config.get_track_name_number()) {
		char num[64], fmt[10];
		snprintf(fmt, sizeof(fmt), "%%0%d" PRId64, _session.track_number_decimals());
		snprintf(num, sizeof(num), fmt, tracknumber);
		diskstream_name += num;
		diskstream_name += "_";
	}
	diskstream_name += str;

	if (diskstream_name == _diskstream_name) {
		return true;
	}
	_diskstream_name = diskstream_name;

	_disk_writer->set_write_source_name (diskstream_name);

	boost::shared_ptr<Track> me = boost::dynamic_pointer_cast<Track> (shared_from_this ());

	if (_playlists[data_type()]->all_regions_empty () && _session.playlists->playlists_for_track (me).size() == 1) {
		/* Only rename the diskstream (and therefore the playlist) if
		   a) the playlist has never had a region added to it and
		   b) there is only one playlist for this track.

		   If (a) is not followed, people can get confused if, say,
		   they have notes about a playlist with a given name and then
		   it changes (see mantis #4759).

		   If (b) is not followed, we rename the current playlist and not
		   the other ones, which is a bit confusing (see mantis #4977).
		*/
		_disk_reader->set_name (str);
		_disk_writer->set_name (str);
	}

	for (uint32_t n = 0; n < DataType::num_types; ++n) {
		if (_playlists[n]) {
			_playlists[n]->set_name (str);
		}
	}

	/* save state so that the statefile fully reflects any filename changes */

	if ((ret = Route::set_name (str)) == 0) {
		_session.save_state ("");
	}

	return ret;
}

void
Track::set_latency_compensation (samplecnt_t longest_session_latency)
{
	Route::set_latency_compensation (longest_session_latency);
	_disk_reader->set_roll_delay (_roll_delay);
}

int
Track::no_roll (pframes_t nframes, samplepos_t start_sample, samplepos_t end_sample, bool session_state_changing)
{
	Glib::Threads::RWLock::ReaderLock lm (_processor_lock, Glib::Threads::TRY_LOCK);

	if (!lm.locked()) {
		return 0;
	}

	bool can_record = _session.actively_recording ();

	/* no outputs? nothing to do ... what happens if we have sends etc. ? */

	if (n_outputs().n_total() == 0 && !ARDOUR::Profile->get_mixbus()) {
		//Note: Mixbus has its own output mechanism, so we should operate even if no explicit outputs are assigned
		return 0;
	}

	/* not active ... do the minimum possible by just outputting silence */

	if (!_active) {
		silence (nframes);
		if (_meter_point == MeterInput && ((_monitoring_control->monitoring_choice() & MonitorInput) || _disk_writer->record_enabled())) {
			_meter->reset();
		}
		return 0;
	}

	if (session_state_changing) {
		if (_session.transport_speed() != 0.0f) {
			/* we're rolling but some state is changing (e.g. our
			   disk reader contents) so we cannot use them. Be
			   silent till this is over. Don't declick.

			   XXX note the absurdity of ::no_roll() being called when we ARE rolling!
			*/
			passthru_silence (start_sample, end_sample, nframes, 0);
			return 0;
		}
		/* we're really not rolling, so we're either delivery silence or actually
		   monitoring, both of which are safe to do while session_state_changing is true.
		*/
	}

	_disk_writer->check_record_status (start_sample, can_record);

	bool be_silent;

	MonitorState const s = monitoring_state ();
	/* we are not rolling, so be silent even if we are monitoring disk, as there
	   will be no disk data coming in.
	*/
	switch (s) {
	case MonitoringSilence:
		be_silent = true;
		break;
	case MonitoringDisk:
		be_silent = true;
		break;
	case MonitoringInput:
		be_silent = false;
		break;
	default:
		be_silent = false;
		break;
	}

	//if we have an internal generator, let it play regardless of monitoring state
	if (_have_internal_generator) {
		be_silent = false;
	}

	_amp->apply_gain_automation (false);

	/* if have_internal_generator, or .. */

	if (be_silent) {

		if (_meter_point == MeterInput) {
			/* still need input monitoring and metering */

			bool const track_rec = _disk_writer->record_enabled ();
			bool const auto_input = _session.config.get_auto_input ();
			bool const software_monitor = Config->get_monitoring_model() == SoftwareMonitoring;
			bool const tape_machine_mode = Config->get_tape_machine_mode ();
			bool no_meter = false;

			/* this needs a proper K-map
			 * and should be separated into a function similar to monitoring_state()
			 * that also handles roll() states in audio_track.cc, midi_track.cc and route.cc
			 *
			 * see http://www.oofus.co.uk/ardour/Ardour3MonitorModesV3.pdf
			 */
			if (!auto_input && !track_rec) {
				no_meter=true;
			}
			else if (tape_machine_mode && !track_rec && auto_input) {
				no_meter=true;
			}
			else if (!software_monitor && tape_machine_mode && !track_rec) {
				no_meter=true;
			}
			else if (!software_monitor && !tape_machine_mode && !track_rec && !auto_input) {
				no_meter=true;
			}

			if (no_meter) {
				BufferSet& bufs (_session.get_silent_buffers (n_process_buffers()));
				_meter->run (bufs, start_sample, end_sample, 1.0, nframes, true);
				_input->process_input (boost::shared_ptr<Processor>(), start_sample, end_sample, _session.transport_speed(), nframes);
			} else {
				_input->process_input (_meter, start_sample, end_sample, _session.transport_speed(), nframes);
			}
		}

		passthru_silence (start_sample, end_sample, nframes, 0);

	} else {

		BufferSet& bufs = _session.get_route_buffers (n_process_buffers());

		fill_buffers_with_input (bufs, _input, nframes);

		if (_meter_point == MeterInput) {
			_meter->run (bufs, start_sample, end_sample, _session.transport_speed(), nframes, true);
		}

		passthru (bufs, start_sample, end_sample, nframes, false);
	}

	flush_processor_buffers_locked (nframes);

	return 0;
}

boost::shared_ptr<Playlist>
Track::playlist ()
{
	return _playlists[data_type()];
}

void
Track::request_input_monitoring (bool m)
{
	for (PortSet::iterator i = _input->ports().begin(); i != _input->ports().end(); ++i) {
		AudioEngine::instance()->request_input_monitoring ((*i)->name(), m);
	}
}

void
Track::ensure_input_monitoring (bool m)
{
	for (PortSet::iterator i = _input->ports().begin(); i != _input->ports().end(); ++i) {
		AudioEngine::instance()->ensure_input_monitoring ((*i)->name(), m);
	}
}

bool
Track::destructive () const
{
	return _disk_writer->destructive ();
}

list<boost::shared_ptr<Source> > &
Track::last_capture_sources ()
{
	return _disk_writer->last_capture_sources ();
}

void
Track::update_latency_information ()
{
	Glib::Threads::RWLock::ReaderLock lr (_processor_lock);
	samplecnt_t chain_latency = _input->latency ();

	for (ProcessorList::iterator p = _processors.begin(); p != _processors.end(); ++p) {
		(*p)->set_input_latency (chain_latency);
		chain_latency += (*p)->signal_latency ();
	}
}

std::string
Track::steal_write_source_name()
{
        return _disk_writer->steal_write_source_name ();
}

void
Track::reset_write_sources (bool r, bool force)
{
	_disk_writer->reset_write_sources (r, force);
}

float
Track::playback_buffer_load () const
{
	return _disk_reader->buffer_load ();
}

float
Track::capture_buffer_load () const
{
	return _disk_writer->buffer_load ();
}

int
Track::do_refill ()
{
	return _disk_reader->do_refill ();
}

int
Track::do_flush (RunContext c, bool force)
{
	return _disk_writer->do_flush (c, force);
}

void
Track::set_pending_overwrite (bool o)
{
	_disk_reader->set_pending_overwrite (o);
}

int
Track::seek (samplepos_t p, bool complete_refill)
{
	if (_disk_reader->seek (p, complete_refill)) {
		return -1;
	}
	return _disk_writer->seek (p, complete_refill);
}

int
Track::can_internal_playback_seek (samplecnt_t p)
{
	return _disk_reader->can_internal_playback_seek (p);
}

int
Track::internal_playback_seek (samplecnt_t p)
{
	return _disk_reader->internal_playback_seek (p);
}

void
Track::non_realtime_locate (samplepos_t p)
{
	Route::non_realtime_locate (p);

	if (!is_private_route()) {
		/* don't waste i/o cycles and butler calls
		   for private tracks (e.g.auditioner)
		*/
		_disk_reader->non_realtime_locate (p);
		_disk_writer->non_realtime_locate (p);
	}
}

void
Track::non_realtime_speed_change ()
{
	_disk_reader->non_realtime_speed_change ();
}

int
Track::overwrite_existing_buffers ()
{
	return _disk_reader->overwrite_existing_buffers ();
}

samplecnt_t
Track::get_captured_samples (uint32_t n) const
{
	return _disk_writer->get_captured_samples (n);
}

int
Track::set_loop (Location* l)
{
	if (_disk_reader->set_loop (l)) {
		return -1;
	}
	return _disk_writer->set_loop (l);
}

void
Track::transport_looped (samplepos_t p)
{
	return _disk_writer->transport_looped (p);
}

bool
Track::realtime_speed_change ()
{
	if (_disk_reader->realtime_speed_change ()) {
		return -1;
	}
	return _disk_writer->realtime_speed_change ();
}

void
Track::realtime_handle_transport_stopped ()
{
	Glib::Threads::RWLock::ReaderLock lm (_processor_lock, Glib::Threads::TRY_LOCK);

	if (!lm.locked ()) {
		return;
	}

	for (ProcessorList::iterator i = _processors.begin(); i != _processors.end(); ++i) {
		(*i)->realtime_handle_transport_stopped ();
	}
}

void
Track::transport_stopped_wallclock (struct tm & n, time_t t, bool g)
{
	_disk_writer->transport_stopped_wallclock (n, t, g);
}

bool
Track::pending_overwrite () const
{
	return _disk_reader->pending_overwrite ();
}

void
Track::prepare_to_stop (samplepos_t t, samplepos_t a)
{
	_disk_writer->prepare_to_stop (t, a);
}

void
Track::set_slaved (bool s)
{
	_disk_reader->set_slaved (s);
	_disk_writer->set_slaved (s);
}

ChanCount
Track::n_channels ()
{
	return _disk_reader->output_streams();
}

samplepos_t
Track::get_capture_start_sample (uint32_t n) const
{
	return _disk_writer->get_capture_start_sample (n);
}

AlignStyle
Track::alignment_style () const
{
	return _disk_writer->alignment_style ();
}

AlignChoice
Track::alignment_choice () const
{
	return _disk_writer->alignment_choice ();
}

samplepos_t
Track::current_capture_start () const
{
	return _disk_writer->current_capture_start ();
}

samplepos_t
Track::current_capture_end () const
{
	return _disk_writer->current_capture_end ();
}

void
Track::playlist_modified ()
{
	_disk_reader->playlist_modified ();
}

int
Track::find_and_use_playlist (DataType dt, PBD::ID const & id)
{
	boost::shared_ptr<Playlist> playlist;

	if ((playlist = _session.playlists->by_id (id)) == 0) {
		return -1;
	}

	if (!playlist) {
		error << string_compose(_("DiskIOProcessor: \"%1\" isn't an playlist"), id.to_s()) << endmsg;
		return -1;
	}

	return use_playlist (dt, playlist);
}

int
Track::use_playlist (DataType dt, boost::shared_ptr<Playlist> p)
{
	int ret;

	if ((ret = _disk_reader->use_playlist (dt, p)) == 0) {
		if ((ret = _disk_writer->use_playlist (dt, p)) == 0) {
			p->set_orig_track_id (id());
		}
	}

	if (ret == 0) {
		_playlists[dt] = p;
	}

	return ret;
}

int
Track::use_copy_playlist ()
{
	assert (_playlists[data_type()]);

	if (_playlists[data_type()] == 0) {
		error << string_compose(_("DiskIOProcessor %1: there is no existing playlist to make a copy of!"), _name) << endmsg;
		return -1;
	}

	string newname;
	boost::shared_ptr<Playlist> playlist;

	newname = Playlist::bump_name (_playlists[data_type()]->name(), _session);

	if ((playlist = PlaylistFactory::create (_playlists[data_type()], newname)) == 0) {
		return -1;
	}

	playlist->reset_shares();

	return use_playlist (data_type(), playlist);
}

int
Track::use_new_playlist (DataType dt)
{
	string newname;
	boost::shared_ptr<Playlist> playlist = _playlists[dt];

	if (playlist) {
		newname = Playlist::bump_name (playlist->name(), _session);
	} else {
		newname = Playlist::bump_name (_name, _session);
	}

	playlist = PlaylistFactory::create (dt, _session, newname, is_private_route());

	if (!playlist) {
		return -1;
	}

	return use_playlist (dt, playlist);
}

void
Track::set_align_choice (AlignChoice ac, bool force)
{
	switch (ac) {
	case Automatic:
		_alignment_choice = Automatic;
		set_align_choice_from_io ();
		return;
	default:
		break;
	}

	_disk_writer->set_align_choice (ac, force);
	_alignment_choice = ac;
}

void
Track::set_align_style (AlignStyle s, bool force)
{
	_disk_writer->set_align_style (s, force);
}

void
Track::set_align_choice_from_io ()
{
	bool have_physical = false;

	if (_input) {
		uint32_t n = 0;
		vector<string> connections;
		boost::shared_ptr<Port> p;

		while (true) {

			p = _input->nth (n++);

			if (!p) {
				break;
			}

			if (p->get_connections (connections) != 0) {
				if (AudioEngine::instance()->port_is_physical (connections[0])) {
					have_physical = true;
					break;
				}
			}

			connections.clear ();
		}
	}

#ifdef MIXBUS
	// compensate for latency when bouncing from master or mixbus.
	// we need to use "ExistingMaterial" to pick up the master bus' latency
	// see also Route::direct_feeds_according_to_reality
	IOVector ios;
	ios.push_back (_input);
	if (_session.master_out() && ios.fed_by (_session.master_out()->output())) {
		have_physical = true;
	}
	for (uint32_t n = 0; n < NUM_MIXBUSES && !have_physical; ++n) {
		if (_session.get_mixbus (n) && ios.fed_by (_session.get_mixbus(n)->output())) {
			have_physical = true;
		}
	}
#endif

	if (have_physical) {
		_disk_writer->set_align_style (ExistingMaterial);
	} else {
		_disk_writer->set_align_style (CaptureTime);
	}
}

void
Track::set_block_size (pframes_t n)
{
	Route::set_block_size (n);
	_disk_reader->set_block_size (n);
	_disk_writer->set_block_size (n);
}

void
Track::adjust_playback_buffering ()
{
        if (_disk_reader) {
                _disk_reader->adjust_buffering ();
        }
}

void
Track::adjust_capture_buffering ()
{
        if (_disk_writer) {
                _disk_writer->adjust_buffering ();
        }
}

#ifdef USE_TRACKS_CODE_FEATURES

/* This is the Tracks version of Track::monitoring_state().
 *
 * Ardour developers: try to flag or fix issues if parts of the libardour API
 * change in ways that invalidate this
 */

MonitorState
Track::monitoring_state () const
{
	/* Explicit requests */

	if (_monitoring != MonitorInput) {
		return MonitoringInput;
	}

	if (_monitoring & MonitorDisk) {
		return MonitoringDisk;
	}

	/* This is an implementation of the truth table in doc/monitor_modes.pdf;
	   I don't think it's ever going to be too pretty too look at.
	*/

	// GZ: NOT USED IN TRACKS
	//bool const auto_input = _session.config.get_auto_input ();
	//bool const software_monitor = Config->get_monitoring_model() == SoftwareMonitoring;
	//bool const tape_machine_mode = Config->get_tape_machine_mode ();

	bool const roll = _session.transport_rolling ();
	bool const track_rec = _diskstream->record_enabled ();
	bool session_rec = _session.actively_recording ();

	if (track_rec) {

		if (!session_rec && roll) {
			return MonitoringDisk;
		} else {
			return MonitoringInput;
		}

	} else {

		if (roll) {
			return MonitoringDisk;
		}
	}

	return MonitoringSilence;
}

#else

/* This is the Ardour/Mixbus version of Track::monitoring_state().
 *
 * Tracks developers: do NOT modify this method under any circumstances.
 */

MonitorState
Track::monitoring_state () const
{
	/* Explicit requests */
	MonitorChoice m (_monitoring_control->monitoring_choice());

	if (m != MonitorAuto) {

		MonitorState ms ((MonitorState) 0);

		if (m & MonitorInput) {
			ms = MonitoringInput;
		}

		if (m & MonitorDisk) {
			ms = MonitorState (ms | MonitoringDisk);
		}

		return ms;
	}

	switch (_session.config.get_session_monitoring ()) {
		case MonitorDisk:
			return MonitoringDisk;
			break;
		case MonitorInput:
			return MonitoringInput;
			break;
		default:
			break;
	}

	/* This is an implementation of the truth table in doc/monitor_modes.pdf;
	   I don't think it's ever going to be too pretty too look at.
	*/

	bool const roll = _session.transport_rolling ();
	bool const track_rec = _disk_writer->record_enabled ();
	bool const auto_input = _session.config.get_auto_input ();
	bool const software_monitor = Config->get_monitoring_model() == SoftwareMonitoring;
	bool const tape_machine_mode = Config->get_tape_machine_mode ();
	bool session_rec;

	/* I suspect that just use actively_recording() is good enough all the
	 * time, but just to keep the semantics the same as they were before
	 * sept 26th 2012, we differentiate between the cases where punch is
	 * enabled and those where it is not.
	 *
	 * rg: I suspect this is not the case: monitoring may differ
	 */

	if (_session.config.get_punch_in() || _session.config.get_punch_out() || _session.preroll_record_punch_enabled ()) {
		session_rec = _session.actively_recording ();
	} else {
		session_rec = _session.get_record_enabled();
	}

	if (track_rec) {

		if (!session_rec && roll && auto_input) {
			return MonitoringDisk;
		} else {
			return software_monitor ? MonitoringInput : MonitoringSilence;
		}

	} else {

		if (tape_machine_mode) {

			return MonitoringDisk;

		} else {

			if (!roll && auto_input) {
				return software_monitor ? MonitoringInput : MonitoringSilence;
			} else {
				return MonitoringDisk;
			}

		}
	}

	abort(); /* NOTREACHED */
	return MonitoringSilence;
}

#endif

void
Track::maybe_declick (BufferSet& bufs, samplecnt_t nframes, int declick)
{
        /* never declick if there is an internal generator - we just want it to
           keep generating sound without interruption.

	   ditto if we are monitoring inputs.
        */

	if (_have_internal_generator || (_monitoring_control->monitoring_choice() == MonitorInput)) {
                return;
        }

        if (!declick) {
		declick = _pending_declick;
	}

	if (declick != 0) {
		Amp::declick (bufs, nframes, declick);
	}
}

samplecnt_t
Track::check_initial_delay (samplecnt_t nframes, samplepos_t& transport_sample)
{
	if (_roll_delay > nframes) {

		_roll_delay -= nframes;
		silence_unlocked (nframes);
		/* transport sample is not legal for caller to use */
		return 0;

	} else if (_roll_delay > 0) {

		nframes -= _roll_delay;
		silence_unlocked (_roll_delay);
		transport_sample += _roll_delay;

		/* shuffle all the port buffers for things that lead "out" of this Route
		   to reflect that we just wrote _roll_delay samples of silence.
		*/

		Glib::Threads::RWLock::ReaderLock lm (_processor_lock);
		for (ProcessorList::iterator i = _processors.begin(); i != _processors.end(); ++i) {
			boost::shared_ptr<IOProcessor> iop = boost::dynamic_pointer_cast<IOProcessor> (*i);
			if (iop) {
				iop->increment_port_buffer_offset (_roll_delay);
			}
		}
		_output->increment_port_buffer_offset (_roll_delay);

		_roll_delay = 0;

	}

	return nframes;
}

void
Track::monitoring_changed (bool, Controllable::GroupControlDisposition)
{
	for (ProcessorList::iterator i = _processors.begin(); i != _processors.end(); ++i) {
		(*i)->monitoring_changed ();
	}
}

MeterState
Track::metering_state () const
{
	bool rv;
	if (_session.transport_rolling ()) {
		// audio_track.cc || midi_track.cc roll() runs meter IFF:
		rv = _meter_point == MeterInput && ((_monitoring_control->monitoring_choice() & MonitorInput) || _disk_writer->record_enabled());
	} else {
		// track no_roll() always metering if
		rv = _meter_point == MeterInput;
	}
	return rv ? MeteringInput : MeteringRoute;
}

bool
Track::set_processor_state (XMLNode const & node, XMLProperty const* prop, ProcessorList& new_order, bool& must_configure)
{
	if (Route::set_processor_state (node, prop, new_order, must_configure)) {
		return true;
	}

	cerr << name() << " looking for state for track procs, DR = " << _disk_reader << endl;

	if (prop->value() == "diskreader") {
		if (_disk_reader) {
			_disk_reader->set_state (node, Stateful::current_state_version);
			new_order.push_back (_disk_reader);
			return true;
		}
	} else if (prop->value() == "diskwriter") {
		if (_disk_writer) {
			_disk_writer->set_state (node, Stateful::current_state_version);
			new_order.push_back (_disk_writer);
			return true;
		}
	}

	error << string_compose(_("unknown Processor type \"%1\"; ignored"), prop->value()) << endmsg;
	return false;
}

void
Track::use_captured_sources (SourceList& srcs, CaptureInfos const & capture_info)
{
	if (srcs.empty()) {
		return;
	}

	boost::shared_ptr<AudioFileSource> afs = boost::dynamic_pointer_cast<AudioFileSource> (srcs.front());
	boost::shared_ptr<SMFSource> mfs = boost::dynamic_pointer_cast<SMFSource> (srcs.front());

	if (afs) {
		use_captured_audio_sources (srcs, capture_info);
	}

	if (mfs) {
		use_captured_midi_sources (srcs, capture_info);
	}
}

void
Track::use_captured_midi_sources (SourceList& srcs, CaptureInfos const & capture_info)
{
	if (srcs.empty() || data_type() != DataType::MIDI) {
		return;
	}

	boost::shared_ptr<SMFSource> mfs = boost::dynamic_pointer_cast<SMFSource> (srcs.front());
	boost::shared_ptr<Playlist> pl = _playlists[DataType::MIDI];
	boost::shared_ptr<MidiRegion> midi_region;
	CaptureInfos::const_iterator ci;

	if (!mfs || !pl) {
		return;
	}

	samplecnt_t total_capture = 0;

	for (total_capture = 0, ci = capture_info.begin(); ci != capture_info.end(); ++ci) {
		total_capture += (*ci)->samples;
	}

	/* we will want to be able to keep (over)writing the source
	   but we don't want it to be removable. this also differs
	   from the audio situation, where the source at this point
	   must be considered immutable. luckily, we can rely on
	   MidiSource::mark_streaming_write_completed() to have
	   already done the necessary work for that.
	*/

	string whole_file_region_name;
	whole_file_region_name = region_name_from_path (mfs->name(), true);

	/* Register a new region with the Session that
	   describes the entire source. Do this first
	   so that any sub-regions will obviously be
	   children of this one (later!)
	*/

	try {
		PropertyList plist;

		plist.add (Properties::name, whole_file_region_name);
		plist.add (Properties::whole_file, true);
		plist.add (Properties::automatic, true);
		plist.add (Properties::start, 0);
		plist.add (Properties::length, total_capture);
		plist.add (Properties::layer, 0);

		boost::shared_ptr<Region> rx (RegionFactory::create (srcs, plist));

		midi_region = boost::dynamic_pointer_cast<MidiRegion> (rx);
		midi_region->special_set_position (capture_info.front()->start);
	}

	catch (failed_constructor& err) {
		error << string_compose(_("%1: could not create region for complete midi file"), _name) << endmsg;
		/* XXX what now? */
	}

	pl->clear_changes ();
	pl->freeze ();

	/* Session sample time of the initial capture in this pass, which is where the source starts */
	samplepos_t initial_capture = 0;
	if (!capture_info.empty()) {
		initial_capture = capture_info.front()->start;
	}

	BeatsSamplesConverter converter (_session.tempo_map(), capture_info.front()->start);
	const samplepos_t preroll_off = _session.preroll_record_trim_len ();

	for (ci = capture_info.begin(); ci != capture_info.end(); ++ci) {

		string region_name;

		RegionFactory::region_name (region_name, mfs->name(), false);

		DEBUG_TRACE (DEBUG::CaptureAlignment, string_compose ("%1 capture start @ %2 length %3 add new region %4\n",
		                                                      _name, (*ci)->start, (*ci)->samples, region_name));


		// cerr << _name << ": based on ci of " << (*ci)->start << " for " << (*ci)->samples << " add a region\n";

		try {
			PropertyList plist;

			/* start of this region is the offset between the start of its capture and the start of the whole pass */
			plist.add (Properties::start, (*ci)->start - initial_capture);
			plist.add (Properties::length, (*ci)->samples);
			plist.add (Properties::length_beats, converter.from((*ci)->samples).to_double());
			plist.add (Properties::name, region_name);

			boost::shared_ptr<Region> rx (RegionFactory::create (srcs, plist));
			midi_region = boost::dynamic_pointer_cast<MidiRegion> (rx);
			if (preroll_off > 0) {
				midi_region->trim_front ((*ci)->start - initial_capture + preroll_off);
			}
		}

		catch (failed_constructor& err) {
			error << _("MidiDiskstream: could not create region for captured midi!") << endmsg;
			continue; /* XXX is this OK? */
		}

		// cerr << "add new region, buffer position = " << buffer_position << " @ " << (*ci)->start << endl;

		pl->add_region (midi_region, (*ci)->start + preroll_off, _disk_writer->non_layered());
	}

	pl->thaw ();
	_session.add_command (new StatefulDiffCommand (pl));
}

void
Track::use_captured_audio_sources (SourceList& srcs, CaptureInfos const & capture_info)
{
	if (srcs.empty() || data_type() != DataType::AUDIO) {
		return;
	}

	boost::shared_ptr<AudioFileSource> afs = boost::dynamic_pointer_cast<AudioFileSource> (srcs.front());
	boost::shared_ptr<Playlist> pl = _playlists[DataType::AUDIO];
	boost::shared_ptr<AudioRegion> region;

	if (!afs || !pl) {
		return;
	}

	/* destructive tracks have a single, never changing region */

	if (destructive()) {

		/* send a signal that any UI can pick up to do the right thing. there is
		   a small problem here in that a UI may need the peak data to be ready
		   for the data that was recorded and this isn't interlocked with that
		   process. this problem is deferred to the UI.
		 */

		pl->LayeringChanged(); // XXX this may not get the UI to do the right thing
		return;
	}

	string whole_file_region_name;
	whole_file_region_name = region_name_from_path (afs->name(), true);

	/* Register a new region with the Session that
	   describes the entire source. Do this first
	   so that any sub-regions will obviously be
	   children of this one (later!)
	*/

	try {
		PropertyList plist;

		plist.add (Properties::start, afs->last_capture_start_sample());
		plist.add (Properties::length, afs->length(0));
		plist.add (Properties::name, whole_file_region_name);
		boost::shared_ptr<Region> rx (RegionFactory::create (srcs, plist));
		rx->set_automatic (true);
		rx->set_whole_file (true);

		region = boost::dynamic_pointer_cast<AudioRegion> (rx);
		region->special_set_position (afs->natural_position());
	}


	catch (failed_constructor& err) {
		error << string_compose(_("%1: could not create region for complete audio file"), _name) << endmsg;
		/* XXX what now? */
	}

	pl->clear_changes ();
	pl->set_capture_insertion_in_progress (true);
	pl->freeze ();

	const samplepos_t preroll_off = _session.preroll_record_trim_len ();
	samplecnt_t buffer_position = afs->last_capture_start_sample ();
	CaptureInfos::const_iterator ci;

	for (ci = capture_info.begin(); ci != capture_info.end(); ++ci) {

		string region_name;

		RegionFactory::region_name (region_name, whole_file_region_name, false);

		DEBUG_TRACE (DEBUG::CaptureAlignment, string_compose ("%1 capture bufpos %5 start @ %2 length %3 add new region %4\n",
		                                                      _name, (*ci)->start, (*ci)->samples, region_name, buffer_position));

		try {

			PropertyList plist;

			plist.add (Properties::start, buffer_position);
			plist.add (Properties::length, (*ci)->samples);
			plist.add (Properties::name, region_name);

			boost::shared_ptr<Region> rx (RegionFactory::create (srcs, plist));
			region = boost::dynamic_pointer_cast<AudioRegion> (rx);
			if (preroll_off > 0) {
				region->trim_front (buffer_position + preroll_off);
			}
		}

		catch (failed_constructor& err) {
			error << _("AudioDiskstream: could not create region for captured audio!") << endmsg;
			continue; /* XXX is this OK? */
		}

		pl->add_region (region, (*ci)->start + preroll_off, 1, _disk_writer->non_layered());
		pl->set_layer (region, DBL_MAX);

		buffer_position += (*ci)->samples;
	}

	pl->thaw ();
	pl->set_capture_insertion_in_progress (false);
	_session.add_command (new StatefulDiffCommand (pl));
}


