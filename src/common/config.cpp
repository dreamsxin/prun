#include <boost/property_tree/json_parser.hpp>
#include <fstream>
#include "config.h"
#include "log.h"

using namespace python_server;

const char Config::defaultCfgName[] = "main.cfg";


bool Config::ParseConfig( const char *cfgPath, const char *cfgName )
{
    configPath_ = cfgPath;
    if ( !configPath_.empty() )
        configPath_ += '/';
    configPath_ += cfgName;

    std::ifstream file( configPath_.c_str() );
    if ( !file.is_open() )
    {
        PS_LOG( "Config::ParseConfig: couldn't open " << configPath_ );
        return false;
    }
    try
    {
        boost::property_tree::read_json( file, ptree_ );
    }
    catch( std::exception &e )
    {
        PS_LOG( "Config::ParseConfig: " << e.what() );
        return false;
    }

    return true;
}
