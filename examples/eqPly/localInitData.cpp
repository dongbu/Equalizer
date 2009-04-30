
/*
 * Copyright (c) 2007-2008, Stefan Eilemann <eile@equalizergraphics.com> 
 *
 * This library is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version.
 *  
 * This library is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more
 * details.
 * 
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA. 
 */

#include "localInitData.h"
#include "frameData.h"

#include <algorithm>
#include <cctype>
#include <functional>

#ifndef MIN
#  define MIN EQ_MIN
#endif
#include <tclap/CmdLine.h>

using namespace std;

namespace eqPly
{
LocalInitData::LocalInitData() 
        : _maxFrames( 0xffffffffu )
        , _color( true )
        , _isResident( false )
{
    _filenames.push_back( "examples/eqPly/rockerArm.ply" );
    _filenames.push_back( "../examples/eqPly/rockerArm.ply" );
    _filenames.push_back( "../share/data/rockerArm.ply" );
    _filenames.push_back( "/opt/local/share/data/rockerArm.ply" );
    _filenames.push_back( "/usr/local/share/data/rockerArm.ply" );
    _filenames.push_back( "/usr/share/data/rockerArm.ply" );
}

const LocalInitData& LocalInitData::operator = ( const LocalInitData& from )
{
    _trackerPort = from._trackerPort;  
    _maxFrames   = from._maxFrames;    
    _color       = from._color;        
    _isResident  = from._isResident;
    _filenames    = from._filenames;
    _pathFilename = from._pathFilename;

    setWindowSystem( from.getWindowSystem( ));
    setRenderMode( from.getRenderMode( ));
    if( from.useGLSL( )) 
        enableGLSL();
    if( from.useInvertedFaces( )) 
        enableInvertedFaces();
    return *this;
}

void LocalInitData::parseArguments( const int argc, char** argv )
{
    try
    {
        string wsHelp = "Window System API ( one of: ";
#ifdef AGL
        wsHelp += "AGL ";
#endif
#ifdef GLX
        wsHelp += "glX ";
#endif
#ifdef WGL
        wsHelp += "WGL ";
#endif
        wsHelp += ")";

        const std::string& desc = EqPly::getHelp();

        TCLAP::CmdLine command( desc );
        TCLAP::MultiArg<string> modelArg( "m", "model", "ply model file name", 
                                          false, "string", command );
        TCLAP::ValueArg<string> portArg( "p", "port", "tracking device port",
                                         false, "/dev/ttyS0", "string",
                                         command );
        TCLAP::SwitchArg colorArg( "b", "bw", "Don't use colors from ply file", 
                                   command, false );
        TCLAP::SwitchArg residentArg( "r", "resident", 
           "Keep client resident (see resident node documentation on website)", 
                                      command, false );
        TCLAP::ValueArg<uint32_t> framesArg( "n", "numFrames", 
                                           "Maximum number of rendered frames", 
                                             false, 0xffffffffu, "unsigned",
                                             command );
        TCLAP::ValueArg<string> wsArg( "w", "windowSystem", wsHelp,
                                       false, "auto", "string", command );
        TCLAP::ValueArg<string> modeArg( "c", "renderMode", 
                                 "Rendering Mode (immediate, displayList, VBO)",
                                       false, "auto", "string", command );
        TCLAP::SwitchArg glslArg( "g", "glsl", "Enable GLSL shaders", 
                                    command, false );
        TCLAP::SwitchArg invFacesArg( "i", "iface",
                            "Invert faces (valid during binary file creation)", 
                                    command, false );
        TCLAP::ValueArg<string> logArg( "l", "log", "output log file",
                                        false, "eqPly.log", "string",
                                        command );
        TCLAP::ValueArg<string> pthArg( "t", "pf", "File with path",
                                       false, "", "string", command );
                                        
        command.parse( argc, argv );

        if( modelArg.isSet( ))
        {
            _filenames.clear();
            _filenames = modelArg.getValue();
        }
        if( portArg.isSet( ))
            _trackerPort = portArg.getValue();
        if( wsArg.isSet( ))
        {
            string windowSystem = wsArg.getValue();
            transform( windowSystem.begin(), windowSystem.end(),
                       windowSystem.begin(), (int(*)(int))std::tolower );

            if( windowSystem == "glx" )
                setWindowSystem( eq::WINDOW_SYSTEM_GLX );
            else if( windowSystem == "agl" )
                setWindowSystem( eq::WINDOW_SYSTEM_AGL );
            else if( windowSystem == "wgl" )
                setWindowSystem( eq::WINDOW_SYSTEM_WGL );
        }

        _color = !colorArg.isSet();

        if( framesArg.isSet( ))
            _maxFrames = framesArg.getValue();

        if( residentArg.isSet( ))
            _isResident = true;

        if( modeArg.isSet() )
        {
            string mode = modeArg.getValue();
            transform( mode.begin(), mode.end(), mode.begin(),
                       (int(*)(int))std::tolower );
            
            if( mode == "immediate" )
                setRenderMode( mesh::RENDER_MODE_IMMEDIATE );
            else if( mode == "displaylist" )
                setRenderMode( mesh::RENDER_MODE_DISPLAY_LIST );
            else if( mode == "vbo" )
                setRenderMode( mesh::RENDER_MODE_BUFFER_OBJECT );
        }

        if( pthArg.isSet( ))
            _pathFilename = pthArg.getValue();

        if( logArg.isSet( ))
            _logFilename = logArg.getValue();
        
        if( glslArg.isSet() )
            enableGLSL();
        if( invFacesArg.isSet() )
            enableInvertedFaces();
    }
    catch( TCLAP::ArgException& exception )
    {
        EQERROR << "Command line parse error: " << exception.error() 
                << " for argument " << exception.argId() << endl;
    }
}
}

