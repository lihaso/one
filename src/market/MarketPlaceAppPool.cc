/* -------------------------------------------------------------------------- */
/* Copyright 2002-2015, OpenNebula Project, OpenNebula Systems                */
/*                                                                            */
/* Licensed under the Apache License, Version 2.0 (the "License"); you may    */
/* not use this file except in compliance with the License. You may obtain    */
/* a copy of the License at                                                   */
/*                                                                            */
/* http://www.apache.org/licenses/LICENSE-2.0                                 */
/*                                                                            */
/* Unless required by applicable law or agreed to in writing, software        */
/* distributed under the License is distributed on an "AS IS" BASIS,          */
/* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.   */
/* See the License for the specific language governing permissions and        */
/* limitations under the License.                                             */
/* -------------------------------------------------------------------------- */

#include "MarketPlaceAppPool.h"
#include "Nebula.h"
#include "Client.h"

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

int MarketPlaceAppPool:: allocate(
            int                uid,
            int                gid,
            const std::string& uname,
            const std::string& gname,
            int                umask,
            MarketPlaceAppTemplate * apptemplate,
            int                mp_id,
            const std::string& mp_name,
            int *              oid,
            std::string&       error_str)
{
    MarketPlaceApp * mp;
    MarketPlaceApp * mp_aux = 0;

    std::string name;

    std::ostringstream oss;

    if (Nebula::instance().is_federation_slave())
    {
        NebulaLog::log("ONE",Log::ERROR,
                "MarketPlaceAppPool::allocate called, but this "
                "OpenNebula is a federation slave");

        return -1;
    }

    mp = new MarketPlaceApp(uid, gid, uname, gname, umask, apptemplate);

    mp->market_id   = mp_id;
    mp->market_name = mp_name;

    mp->state = MarketPlaceApp::INIT;

    mp->remove_template_attribute("IMPORTED");

    // -------------------------------------------------------------------------
    // Check name & duplicates
    // -------------------------------------------------------------------------
    mp->get_template_attribute("NAME", name);

    if ( !PoolObjectSQL::name_is_valid(name, error_str) )
    {
        goto error_name;
    }

    mp_aux = get(name, uid, false);

    if( mp_aux != 0 )
    {
        goto error_duplicated;
    }

    *oid = PoolSQL::allocate(mp, error_str);

    return *oid;

error_duplicated:
    oss << "NAME is already taken by MARKETPLACEAPP " << mp_aux->get_oid();
    error_str = oss.str();

error_name:
    delete mp;
    *oid = -1;

    return *oid;
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

int MarketPlaceAppPool::drop(PoolObjectSQL * objsql, std::string& error_msg)
{
    if (Nebula::instance().is_federation_slave())
    {
        Client * client = Client::client();

        xmlrpc_c::value result;
        vector<xmlrpc_c::value> values;

        std::ostringstream oss;

        try
        {
            client->call(client->get_endpoint(),
                    "one.marketapp.dropdb",
                    "si",
                    &result,
                    client->get_oneauth().c_str(),
                    objsql->get_oid());
        }
        catch (exception const& e)
        {
            oss << "Cannot drop  marketapp in federation master db: "<<e.what();
            NebulaLog::log("MKP", Log::ERROR, oss);

            return -1;
        }

        values = xmlrpc_c::value_array(result).vectorValueValue();

        if ( xmlrpc_c::value_boolean(values[0]) == false )
        {
            std::string error = xmlrpc_c::value_string(values[1]);

            oss << "Cannot drop marketapp in federation master db: " << error;
            NebulaLog::log("MKP", Log::ERROR, oss);
            return -1;
        }

        return 0;
    }

    return PoolSQL::drop(objsql, error_msg);
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

int MarketPlaceAppPool::import(const std::string& t64, int mp_id, int mp_zone_id,
        const std::string& mp_name, std::string& error_str)
{
	// ---------------------------------------------------------------------- //
	// Slave forwards DB import to federation master                          //
	// ---------------------------------------------------------------------- //
    if (Nebula::instance().is_federation_slave())
    {
        Client * client = Client::client();

        xmlrpc_c::value result;
        vector<xmlrpc_c::value> values;

        std::ostringstream oss;

        try
        {
            client->call(client->get_endpoint(),
                    "one.marketapp.allocatedb",
                    "si",
                    &result,
                    client->get_oneauth().c_str(),
					t64.c_str(),
                    mp_id);
        }
        catch (exception const& e)
        {
            oss << "Cannot import  marketapp in federation master db: "<<e.what();
            NebulaLog::log("MKP", Log::ERROR, oss);

            return -1;
        }

        values = xmlrpc_c::value_array(result).vectorValueValue();

        if ( xmlrpc_c::value_boolean(values[0]) == false )
        {
            std::string error = xmlrpc_c::value_string(values[1]);

            oss << "Cannot import marketapp in federation master db: " << error;
            NebulaLog::log("MKP", Log::ERROR, oss);
            return -1;
        }

        return 0;
    }

	// ---------------------------------------------------------------------- //
	// Master import logic                                                    //
	// ---------------------------------------------------------------------- //
    MarketPlaceApp * app = new MarketPlaceApp(UserPool::ONEADMIN_ID,
        GroupPool::ONEADMIN_ID, UserPool::oneadmin_name,
        GroupPool::ONEADMIN_NAME, 0133, 0);

    int rc = app->from_template64(t64, error_str);

    if ( rc != 0 )
    {
        delete app;
        return -1;
    }

    app->market_id   = mp_id;
    app->market_name = mp_name;
	app->zone_id     = mp_zone_id;

    if ( !PoolObjectSQL::name_is_valid(app->name, error_str) )
    {
        std::ostringstream oss;

        oss << "imported-" << app->get_origin_id();
        app->name = oss.str();

        if ( !PoolObjectSQL::name_is_valid(app->name, error_str) )
        {
            error_str = "Cannot generate a valida name for app";
            return -1;
        }
    }

    MarketPlaceApp * mp_aux = get(app->name, 0, false);

    if( mp_aux != 0 )
    {
        //Marketplace app already imported
        delete app;
        return -2;
    }

    return PoolSQL::allocate(app, error_str);
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

int MarketPlaceAppPool::update(PoolObjectSQL * objsql)
{
    if (Nebula::instance().is_federation_slave())
    {
        std::string tmpl_xml;
        Client * client = Client::client();

        xmlrpc_c::value result;
        vector<xmlrpc_c::value> values;

        std::ostringstream oss;

        try
        {
            client->call(client->get_endpoint(),
                    "one.marketapp.updatedb",
                    "sis",
                    &result,
                    client->get_oneauth().c_str(),
                    objsql->get_oid(),
                    objsql->to_xml(tmpl_xml).c_str());
        }
        catch (exception const& e)
        {
            oss << "Cannot update marketapp in federation master db: "<<e.what();
            NebulaLog::log("MKP", Log::ERROR, oss);

            return -1;
        }

        values = xmlrpc_c::value_array(result).vectorValueValue();

        if ( xmlrpc_c::value_boolean(values[0]) == false )
        {
            std::string error = xmlrpc_c::value_string(values[1]);

            oss << "Cannot update marketapp in federation master db: " << error;
            NebulaLog::log("MKP", Log::ERROR, oss);

            return -1;
        }

        return 0;
    }

    return PoolSQL::update(objsql);
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

