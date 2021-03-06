
/*******************************************************************************/
/* Copyright (C) 2009 Jonathan Moore Liles                                     */
/*                                                                             */
/* This program is free software; you can redistribute it and/or modify it     */
/* under the terms of the GNU General Public License as published by the       */
/* Free Software Foundation; either version 2 of the License, or (at your      */
/* option) any later version.                                                  */
/*                                                                             */
/* This program is distributed in the hope that it will be useful, but WITHOUT */
/* ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or       */
/* FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for   */
/* more details.                                                               */
/*                                                                             */
/* You should have received a copy of the GNU General Public License along     */
/* with This program; see the file COPYING.  If not,write to the Free Software */
/* Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.  */
/*******************************************************************************/

/* Filter module. Can host LADPSA Plugins, or can be inherited from to make internal
   modules with special features and appearance. */

#include "const.h"

#include <string.h>
#include <vector>
#include <string>
#include <ladspa.h>
#include <stdlib.h>
#include <math.h>

#include <FL/fl_draw.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Menu_Button.H>

#include "Plugin_Module.H"

#include "debug.h"

#define HAVE_LIBLRDF 1
#include "LADSPAInfo.h"

#include "Chain.H"
//#include "Client/Client.H"

#include <dsp.h>

#include <algorithm>



static LADSPAInfo *ladspainfo;
Thread* Plugin_Module::plugin_discover_thread;

/* keep this out of the header to avoid spreading ladspa.h dependency */
struct Plugin_Module::ImplementationData
{
    const LADSPA_Descriptor     *descriptor;
//    std::vector<LADSPA_Data*>    m_LADSPABufVec;
    std::vector<LADSPA_Handle>   handle;
};



Plugin_Module::Plugin_Module ( ) : Module( 50, 35, name() )
{
    init();

    color( fl_color_average(  fl_rgb_color( 0x99, 0x7c, 0x3a ), FL_BACKGROUND_COLOR, 1.0f ));
    
    end();

    log_create();
}

Plugin_Module::~Plugin_Module ( )
{
    log_destroy();
    plugin_instances( 0 );
}



void
Plugin_Module::get ( Log_Entry &e ) const
{
//    char s[512];
//    snprintf( s, sizeof( s ), "ladspa:%lu", _idata->descriptor->UniqueID );
    e.add( ":plugin_id", _idata->descriptor->UniqueID );

    /* these help us display the module on systems which are missing this plugin */
    e.add( ":plugin_ins", _plugin_ins );
    e.add( ":plugin_outs", _plugin_outs );

    Module::get( e );
}

void
Plugin_Module::set ( Log_Entry &e )
{
    int n = 0;

    /* we need to have number() defined before we create the control inputs in load() */
    for ( int i = 0; i < e.size(); ++i )
    {
        const char *s, *v;
	
        e.get( i, &s, &v );

    	if ( ! strcmp(s, ":number" ) )
	{
	    n = atoi(v);
	}
    }
    
    /* need to call this to set label even for version 0 modules */
    number(n);
    
    for ( int i = 0; i < e.size(); ++i )
    {
        const char *s, *v;

        e.get( i, &s, &v );

        if ( ! strcmp( s, ":plugin_id" ) )
        {
            load( (unsigned long) atoll ( v ) );
        }
        else if ( ! strcmp( s, ":plugin_ins" ) )
        {
            _plugin_ins = atoi( v );
        }
        else if ( ! strcmp( s, ":plugin_outs" ) )
        {
            _plugin_outs = atoi( v );
        }
    }

    Module::set( e );
}



void
Plugin_Module::init ( void )
{
    _latency = 0;
    _last_latency = 0;
    _idata = new Plugin_Module::ImplementationData();
    _idata->handle.clear();
    /* module will be bypassed until plugin is loaded */
    _bypass = true;
    _crosswire = false;

    align( (Fl_Align)FL_ALIGN_CENTER | FL_ALIGN_INSIDE );
//     color( (Fl_Color)fl_color_average( FL_MAGENTA, FL_WHITE, 0.5f ) );

    int tw, th, tx, ty;

    bbox( tx, ty, tw, th );
}

void
Plugin_Module::update ( void )
{
    if ( _last_latency != _latency )
    {
        DMESSAGE( "Plugin latency changed to %lu", (unsigned long)_latency );

        chain()->client()->recompute_latencies();
    }

    _last_latency = _latency;

    update_tooltip();
}

int
Plugin_Module::can_support_inputs ( int n )
{
    /* this is the simple case */
    if ( plugin_ins() == n )
        return plugin_outs();
    /* e.g. MONO going into STEREO */
    /* we'll duplicate our inputs */
    else if ( n < plugin_ins() &&
              1 == n  )
    {
        return plugin_outs();
    }
    /* e.g. STEREO going into MONO */
    /* we'll run multiple instances of the plugin */
    else if ( n > plugin_ins() &&
              ( plugin_ins() == 1 && plugin_outs() == 1 ) )
    {
        return n;
    }

    return -1;
}

bool
Plugin_Module::configure_inputs( int n )
{
    unsigned int inst = _idata->handle.size();

    if ( ninputs() != n )
    {
        _crosswire = false;

        if ( n != ninputs() )
        {
            if ( 1 == n && plugin_ins() > 1 )
            {
                DMESSAGE( "Cross-wiring plugin inputs" );
                _crosswire = true;

                audio_input.clear();

                for ( int i = n; i--; )
                    audio_input.push_back( Port( this, Port::INPUT, Port::AUDIO ) );
            }
            else if ( n >= plugin_ins() &&
                      ( plugin_ins() == 1 && plugin_outs() == 1 ) )
            {
                DMESSAGE( "Running multiple instances of plugin" );

                audio_input.clear();
                audio_output.clear();

                for ( int i = n; i--; )
                {
                    add_port( Port( this, Port::INPUT, Port::AUDIO ) );
                    add_port( Port( this, Port::OUTPUT, Port::AUDIO ) );
                }

                inst = n;
            }
            else if ( n == plugin_ins() )
            {
                DMESSAGE( "Plugin input configuration is a perfect match" );
            }
            else
            {
                DMESSAGE( "Unsupported input configuration" );
                return false;
            }
        }
    }

    if ( loaded() )
    {
        bool b = bypass();
        if ( inst != _idata->handle.size() )
        {
            if ( !b )
                deactivate();
            
            if ( plugin_instances( inst ) )
                instances( inst );
            else
                return false;
            
            if ( !b )
                activate();
        }
    }

    return true;
}

void *
Plugin_Module::discover_thread ( void * )
{
    THREAD_ASSERT( Plugin_Discover );

    DMESSAGE( "Discovering plugins in the background" );

    ladspainfo = new LADSPAInfo();

    return NULL;
}

/* Spawn a background thread for plugin discovery */
void
Plugin_Module::spawn_discover_thread ( void )
{
    if ( plugin_discover_thread )
    {
        FATAL( "Plugin discovery thread is already running or has completed" );
    }

    plugin_discover_thread = new Thread( "Plugin_Discover" );

    plugin_discover_thread->clone( &Plugin_Module::discover_thread, NULL );
}

void
Plugin_Module::join_discover_thread ( void )
{
    plugin_discover_thread->join();
}

/* return a list of available plugins */
std::list<Plugin_Module::Plugin_Info>
Plugin_Module::get_all_plugins ( void )
{
    if ( !ladspainfo )
    {
        if ( ! plugin_discover_thread )
            ladspainfo = new LADSPAInfo();
        else
            plugin_discover_thread->join();
    }

    std::vector<LADSPAInfo::PluginInfo> plugins = ladspainfo->GetPluginInfo();

    std::list<Plugin_Module::Plugin_Info> pr;

    int j = 0;
    for (std::vector<LADSPAInfo::PluginInfo>::iterator i=plugins.begin();
         i!=plugins.end(); i++, j++)
    {
        Plugin_Info pi;

        //   pi[j].path = i->Name.c_str();
        pi.path = NULL;
        pi.id = i->UniqueID;
        pi.author = i->Maker;
        pi.name = i->Name;
        pi.audio_inputs = i->AudioInputs;
        pi.audio_outputs = i->AudioOutputs;
        pi.category = "Unclassified";
        pr.push_back( pi );
    }

    pr.sort();

    const std::vector<LADSPAInfo::PluginEntry> pe = ladspainfo->GetMenuList();
  
    for (std::vector<LADSPAInfo::PluginEntry>::const_iterator i= pe.begin();
         i !=pe.end(); i++ )
    {
        for ( std::list<Plugin_Info>::iterator j = pr.begin(); j != pr.end(); j++ )
        {
            if ( j->id == i->UniqueID )
            {
                j->category = i->Category;
            }
        }
    }

    return pr;
}

bool
Plugin_Module::plugin_instances ( unsigned int n )
{
    if ( _idata->handle.size() > n )
    {
        for ( int i = _idata->handle.size() - n; i--; )
        {
            DMESSAGE( "Destroying plugin instance" );

            LADSPA_Handle h = _idata->handle.back();

            if ( _idata->descriptor->deactivate )
                _idata->descriptor->deactivate( h );
            if ( _idata->descriptor->cleanup )
                _idata->descriptor->cleanup( h );

            _idata->handle.pop_back();
        }
    }
    else if ( _idata->handle.size() < n )
    {
        for ( int i = n - _idata->handle.size(); i--; )
        {
            LADSPA_Handle h;

            DMESSAGE( "Instantiating plugin... with sample rate %lu", (unsigned long)sample_rate());

            if ( ! (h = _idata->descriptor->instantiate( _idata->descriptor, sample_rate() ) ) )
            {
                WARNING( "Failed to instantiate plugin" );
                return false;
            }

            DMESSAGE( "Instantiated: %p", h );

            _idata->handle.push_back( h );

            DMESSAGE( "Connecting control ports..." );

            int ij = 0;
            int oj = 0;
            for ( unsigned int k = 0; k < _idata->descriptor->PortCount; ++k )
            {
                if ( LADSPA_IS_PORT_CONTROL( _idata->descriptor->PortDescriptors[k] ) )
                {
                    if ( LADSPA_IS_PORT_INPUT( _idata->descriptor->PortDescriptors[k] ) )
                        _idata->descriptor->connect_port( h, k, (LADSPA_Data*)control_input[ij++].buffer() );
                    else if ( LADSPA_IS_PORT_OUTPUT( _idata->descriptor->PortDescriptors[k] ) )
                        _idata->descriptor->connect_port( h, k, (LADSPA_Data*)control_output[oj++].buffer() );
                }
            }

            // connect ports to magic bogus value to aid debugging.
            for ( unsigned int k = 0; k < _idata->descriptor->PortCount; ++k )
                if ( LADSPA_IS_PORT_AUDIO( _idata->descriptor->PortDescriptors[k] ) )
                    _idata->descriptor->connect_port( h, k, (LADSPA_Data*)0x42 );

        }
    }

    return true;
}

void
Plugin_Module::bypass ( bool v )
{
    if ( v != bypass() )
    {
        if ( v )
            deactivate();
        else
            activate();
    }
}

nframes_t
Plugin_Module::get_module_latency ( void ) const
{
    for ( unsigned int i = ncontrol_outputs(); i--; )
    {
        if ( !strcasecmp( "latency", control_output[i].name() ) )
        {
            return control_output[i].control_value();
        }
    } 
    
    return 0;
}

bool
Plugin_Module::load ( unsigned long id )
{
    if ( !ladspainfo )
    {
        if ( ! plugin_discover_thread )
            ladspainfo = new LADSPAInfo();
        else
            plugin_discover_thread->join();
    }

    _idata->descriptor = ladspainfo->GetDescriptorByID( id );

    _plugin_ins = _plugin_outs = 0;

    if ( ! _idata->descriptor )
    {
        /* unknown plugin ID */
        WARNING( "Unknown plugin ID: %lu", id );
	char s[25];

	snprintf( s, 24, "! %lu", id );
	
        base_label( s );
        return false;
    }

    base_label( _idata->descriptor->Name );

    if ( _idata->descriptor )
    {
        if ( LADSPA_IS_INPLACE_BROKEN( _idata->descriptor->Properties ) )
        {
            WARNING( "Cannot use this plugin because it is incapable of processing audio in-place" );
            return false;
        }
       
        /* else if ( ! LADSPA_IS_HARD_RT_CAPABLE( _idata->descriptor->Properties ) ) */
        /* { */
        /*     WARNING( "Cannot use this plugin because it is incapable of hard real-time operation" ); */
        /*     return false; */
        /* } */

        MESSAGE( "Name: %s", _idata->descriptor->Name );

        for ( unsigned int i = 0; i < _idata->descriptor->PortCount; ++i )
        {
            if ( LADSPA_IS_PORT_AUDIO( _idata->descriptor->PortDescriptors[i] ) )
            {
                if ( LADSPA_IS_PORT_INPUT( _idata->descriptor->PortDescriptors[i] ) )
                {
                    add_port( Port( this, Port::INPUT, Port::AUDIO, _idata->descriptor->PortNames[ i ] ) );
                    _plugin_ins++;
                }
                else if (LADSPA_IS_PORT_OUTPUT(_idata->descriptor->PortDescriptors[i]))
                {
                    _plugin_outs++;
                    add_port( Port( this, Port::OUTPUT, Port::AUDIO, _idata->descriptor->PortNames[ i ] ) );
                }
            }
        }

        MESSAGE( "Plugin has %i inputs and %i outputs", _plugin_ins, _plugin_outs);

        for ( unsigned int i = 0; i < _idata->descriptor->PortCount; ++i )
        {
            if ( LADSPA_IS_PORT_CONTROL( _idata->descriptor->PortDescriptors[i] ) )
            {
                Port::Direction d = Port::INPUT;

                if ( LADSPA_IS_PORT_INPUT( _idata->descriptor->PortDescriptors[i] ) )
                {
                    d = Port::INPUT;
                }
                else if ( LADSPA_IS_PORT_OUTPUT( _idata->descriptor->PortDescriptors[i] ) )
                {
                    d = Port::OUTPUT;
                }

                Port p( this, d, Port::CONTROL, _idata->descriptor->PortNames[ i ] );

                p.hints.default_value = 0;

                LADSPA_PortRangeHintDescriptor hd = _idata->descriptor->PortRangeHints[i].HintDescriptor;

                if ( LADSPA_IS_HINT_BOUNDED_BELOW(hd) )
                {
                    p.hints.ranged = true;
                    p.hints.minimum = _idata->descriptor->PortRangeHints[i].LowerBound;
                    if ( LADSPA_IS_HINT_SAMPLE_RATE(hd) )
                    {
                        p.hints.minimum *= sample_rate();
                    }
                }
                if ( LADSPA_IS_HINT_BOUNDED_ABOVE(hd) )
                {
                    p.hints.ranged = true;
                    p.hints.maximum = _idata->descriptor->PortRangeHints[i].UpperBound;
                    if ( LADSPA_IS_HINT_SAMPLE_RATE(hd) )
                    {
                        p.hints.maximum *= sample_rate();
                    }
                }

                if ( LADSPA_IS_HINT_HAS_DEFAULT(hd) )
                {

                    float Max=1.0f, Min=-1.0f, Default=0.0f;
                    int Port=i;

                    // Get the bounding hints for the port
                    LADSPA_PortRangeHintDescriptor HintDesc=_idata->descriptor->PortRangeHints[Port].HintDescriptor;
                    if (LADSPA_IS_HINT_BOUNDED_BELOW(HintDesc))
                    {
                        Min=_idata->descriptor->PortRangeHints[Port].LowerBound;
                        if (LADSPA_IS_HINT_SAMPLE_RATE(HintDesc))
                        {
                            Min*=sample_rate();
                        }
                    }
                    if (LADSPA_IS_HINT_BOUNDED_ABOVE(HintDesc))
                    {
                        Max=_idata->descriptor->PortRangeHints[Port].UpperBound;
                        if (LADSPA_IS_HINT_SAMPLE_RATE(HintDesc))
                        {
                            Max*=sample_rate();
                        }
                    }

#ifdef LADSPA_VERSION
// We've got a version of the header that supports port defaults
                    if (LADSPA_IS_HINT_HAS_DEFAULT(HintDesc)) {
                        // LADSPA_HINT_DEFAULT_0 is assumed anyway, so we don't check for it
                        if (LADSPA_IS_HINT_DEFAULT_1(HintDesc)) {
                            Default = 1.0f;
                        } else if (LADSPA_IS_HINT_DEFAULT_100(HintDesc)) {
                            Default = 100.0f;
                        } else if (LADSPA_IS_HINT_DEFAULT_440(HintDesc)) {
                            Default = 440.0f;
                        } else {
                            // These hints may be affected by SAMPLERATE, LOGARITHMIC and INTEGER
                            if (LADSPA_IS_HINT_DEFAULT_MINIMUM(HintDesc) &&
                                LADSPA_IS_HINT_BOUNDED_BELOW(HintDesc)) {
                                Default=_idata->descriptor->PortRangeHints[Port].LowerBound;
                            } else if (LADSPA_IS_HINT_DEFAULT_MAXIMUM(HintDesc) &&
                                       LADSPA_IS_HINT_BOUNDED_ABOVE(HintDesc)) {
                                Default=_idata->descriptor->PortRangeHints[Port].UpperBound;
                            } else if (LADSPA_IS_HINT_BOUNDED_BELOW(HintDesc) &&
                                       LADSPA_IS_HINT_BOUNDED_ABOVE(HintDesc)) {
                                // These hints require both upper and lower bounds
                                float lp = 0.0f, up = 0.0f;
                                float min = _idata->descriptor->PortRangeHints[Port].LowerBound;
                                float max = _idata->descriptor->PortRangeHints[Port].UpperBound;
                                if (LADSPA_IS_HINT_DEFAULT_LOW(HintDesc)) {
                                    lp = 0.75f;
                                    up = 0.25f;
                                } else if (LADSPA_IS_HINT_DEFAULT_MIDDLE(HintDesc)) {
                                    lp = 0.5f;
                                    up = 0.5f;
                                } else if (LADSPA_IS_HINT_DEFAULT_HIGH(HintDesc)) {
                                    lp = 0.25f;
                                    up = 0.75f;
                                }

                                if (LADSPA_IS_HINT_LOGARITHMIC(HintDesc)) {

                                    p.hints.type = Port::Hints::LOGARITHMIC;

                                    if (min==0.0f || max==0.0f) {
                                        // Zero at either end means zero no matter
                                        // where hint is at, since:
                                        //  log(n->0) -> Infinity
                                        Default = 0.0f;
                                    } else {
                                        // Catch negatives
                                        bool neg_min = min < 0.0f ? true : false;
                                        bool neg_max = max < 0.0f ? true : false;

                                        if (!neg_min && !neg_max) {
                                            Default = exp(::log(min) * lp + ::log(max) * up);
                                        } else if (neg_min && neg_max) {
                                            Default = -exp(::log(-min) * lp + ::log(-max) * up);
                                        } else {
                                            // Logarithmic range has asymptote
                                            // so just use linear scale
                                            Default = min * lp + max * up;
                                        }
                                    }
                                } else {
                                    Default = min * lp + max * up;
                                }
                            }
                            if (LADSPA_IS_HINT_SAMPLE_RATE(HintDesc)) {
                                Default *= sample_rate();
                            }
                        }

                        if (LADSPA_IS_HINT_INTEGER(HintDesc)) {
                            if ( p.hints.ranged &&
                                 0 == (int)p.hints.minimum &&
                                 1 == (int)p.hints.maximum )
                                p.hints.type = Port::Hints::BOOLEAN;
                            else
                                p.hints.type = Port::Hints::INTEGER;
                            Default = floorf(Default);
                        }
                        if (LADSPA_IS_HINT_TOGGLED(HintDesc)){
                            p.hints.type = Port::Hints::BOOLEAN;
                        }
                    }
#else
                    Default = 0.0f;
#endif
                    p.hints.default_value = Default;
                }

                float *control_value = new float;

                *control_value = p.hints.default_value;

                p.connect_to( control_value );

                add_port( p );

                DMESSAGE( "Plugin has control port \"%s\" (default: %f)", _idata->descriptor->PortNames[ i ], p.hints.default_value );
            }
        }
    }
    else
    {
        WARNING( "Failed to load plugin" );
        return false;
    }

    int instances = plugin_instances( 1 );
    
    if ( instances )
    {
        bypass( false );
    }

    return instances;
}

void
Plugin_Module::set_input_buffer ( int n, void *buf )
{
    LADSPA_Handle h;

    if ( instances() > 1 )
    {
        h = _idata->handle[n];
        n = 0;
    }
    else
        h = _idata->handle[0];

    for ( unsigned int i = 0; i < _idata->descriptor->PortCount; ++i )
        if ( LADSPA_IS_PORT_INPUT( _idata->descriptor->PortDescriptors[i] ) &&
             LADSPA_IS_PORT_AUDIO( _idata->descriptor->PortDescriptors[i] ) )
            if ( n-- == 0 )
                _idata->descriptor->connect_port( h, i, (LADSPA_Data*)buf );
}

bool
Plugin_Module::loaded ( void ) const
{
    return _idata->descriptor;
}

void
Plugin_Module::set_output_buffer ( int n, void *buf )
{
    LADSPA_Handle h;

    if ( instances() > 1 )
    {
        h = _idata->handle[n];
        n = 0;
    }
    else
        h = _idata->handle[0];

    for ( unsigned int i = 0; i < _idata->descriptor->PortCount; ++i )
        if ( LADSPA_IS_PORT_OUTPUT( _idata->descriptor->PortDescriptors[i] ) &&
             LADSPA_IS_PORT_AUDIO( _idata->descriptor->PortDescriptors[i] ) )
            if ( n-- == 0 )
                _idata->descriptor->connect_port( h, i, (LADSPA_Data*)buf );
}

void
Plugin_Module::activate ( void )
{
    if ( !loaded() )
        return;

    DMESSAGE( "Activating plugin \"%s\"", label() );

    if ( !bypass() )
        FATAL( "Attempt to activate already active plugin" );

    if ( chain() )
        chain()->client()->lock();

    if ( _idata->descriptor->activate )
        for ( unsigned int i = 0; i < _idata->handle.size(); ++i )
            _idata->descriptor->activate( _idata->handle[i] );

    _bypass = false;

    if ( chain() )
        chain()->client()->unlock();
}

void
Plugin_Module::deactivate( void )
{
    if ( !loaded() )
        return;

    DMESSAGE( "Deactivating plugin \"%s\"", label() );

    if ( chain() )
        chain()->client()->lock();

    _bypass = true;
   
    if ( _idata->descriptor->deactivate )
        for ( unsigned int i = 0; i < _idata->handle.size(); ++i )
            _idata->descriptor->deactivate( _idata->handle[i] );

    if ( chain() )
        chain()->client()->unlock();
}

void
Plugin_Module::handle_port_connection_change ( void )
{
//    DMESSAGE( "Connecting audio ports" );

    if ( loaded() )
    {
        if ( _crosswire )
        {
            for ( int i = 0; i < plugin_ins(); ++i )
                set_input_buffer( i, audio_input[0].buffer() );
        }
        else
        {
            for ( unsigned int i = 0; i < audio_input.size(); ++i )
                set_input_buffer( i, audio_input[i].buffer() );
        }

        for ( unsigned int i = 0; i < audio_output.size(); ++i )
            set_output_buffer( i, audio_output[i].buffer() );
    }
}



bool 
Plugin_Module::get_impulse_response ( sample_t *buf, nframes_t nframes )
{
    apply( buf, nframes );
    
    if ( buffer_is_digital_black( buf + 1, nframes - 1 ))
        /* no impulse response... */
        return false;

    return true;
}

/** Instantiate a temporary version of the plugin, and run it (in place) against the provided buffer */
bool
Plugin_Module::apply ( sample_t *buf, nframes_t nframes )
{
// actually osc or UI    THREAD_ASSERT( UI );

    LADSPA_Handle h;

    if ( ! (h = _idata->descriptor->instantiate( _idata->descriptor, sample_rate() ) ) )
    {
        WARNING( "Failed to instantiate plugin" );
        return false;
    }

    int ij = 0;
    int oj = 0;
    for ( unsigned int k = 0; k < _idata->descriptor->PortCount; ++k )
    {
        if ( LADSPA_IS_PORT_CONTROL( _idata->descriptor->PortDescriptors[k] ) )
        {
            if ( LADSPA_IS_PORT_INPUT( _idata->descriptor->PortDescriptors[k] ) )
                _idata->descriptor->connect_port( h, k, (LADSPA_Data*)control_input[ij++].buffer() );
            else if ( LADSPA_IS_PORT_OUTPUT( _idata->descriptor->PortDescriptors[k] ) )
                _idata->descriptor->connect_port( h, k, (LADSPA_Data*)control_output[oj++].buffer() );
        }
    }
    
    
    if ( _idata->descriptor->activate )
        _idata->descriptor->activate( h );

    int tframes = 512;
    float tmp[tframes];

    memset( tmp, 0, sizeof( float ) * tframes );

    for ( unsigned int k = 0; k < _idata->descriptor->PortCount; ++k )
        if ( LADSPA_IS_PORT_AUDIO( _idata->descriptor->PortDescriptors[k] ) )
            _idata->descriptor->connect_port( h, k, tmp );
    

    /* flush any parameter interpolation */
    _idata->descriptor->run( h, tframes );
    
    for ( unsigned int k = 0; k < _idata->descriptor->PortCount; ++k )
        if ( LADSPA_IS_PORT_AUDIO( _idata->descriptor->PortDescriptors[k] ) )
            _idata->descriptor->connect_port( h, k, buf );
   
    /* run for real */
    _idata->descriptor->run( h, nframes );
    
    if ( _idata->descriptor->deactivate )
        _idata->descriptor->deactivate( h );
    if ( _idata->descriptor->cleanup )
        _idata->descriptor->cleanup( h );

    return true;
}
/**********/
/* Client */
/**********/

void
Plugin_Module::process ( nframes_t nframes )
{
    handle_port_connection_change();

    if ( unlikely( bypass() ) )
    {
        /* If this is a mono to stereo plugin, then duplicate the input channel... */
        /* There's not much we can do to automatically support other configurations. */
        if ( ninputs() == 1 && noutputs() == 2 )
        {
            buffer_copy( (sample_t*)audio_output[1].buffer(), (sample_t*)audio_input[0].buffer(), nframes );
        }

        _latency = 0;
    }
    else
    {
        for ( unsigned int i = 0; i < _idata->handle.size(); ++i )
            _idata->descriptor->run( _idata->handle[i], nframes );

        _latency = get_module_latency();
    }
}


