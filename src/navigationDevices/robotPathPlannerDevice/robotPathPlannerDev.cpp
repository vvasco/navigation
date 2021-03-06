/* 
 * Copyright (C)2011  Department of Robotics Brain and Cognitive Sciences - Istituto Italiano di Tecnologia
 * Author: Marco Randazzo
 * email:  marco.randazzo@iit.it
 * website: www.robotcub.org
 * Permission is granted to copy, distribute, and/or modify this program
 * under the terms of the GNU General Public License, version 2 or any
 * later version published by the Free Software Foundation.
 *
 * A copy of the license can be found at
 * http://www.robotcub.org/icub/license/gpl.txt
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details
*/

/**
 * \section robotPathPlanner
 * This module performs global path-planning by generating a sequence of waypoints to be tracked by a local navigation algorithm.
 *  It receives a goal from the user either via RPC command or via yarp iNavigation2D interface and it computes a sequence of waypoints which are sent one by one to a local navigator module.
 * If the local navigation module fails to reach one of these waypoints, the global navigation is aborted too. 
 * A detailed description of configuration parameters available for the module is provided in the README.md file.
 */

#include <yarp/os/Network.h>
#include <yarp/os/RFModule.h>
#include <yarp/os/Time.h>
#include <yarp/os/Port.h>
#include <yarp/dev/ControlBoardInterfaces.h>

#include "robotPathPlannerDev.h"
#include "pathPlannerCtrl.h"
#include <math.h>

using namespace yarp::dev::Nav2D;

robotPathPlannerDev::robotPathPlannerDev()
{
    m_plannerThread=NULL;
}

bool robotPathPlannerDev::open(yarp::os::Searchable& config)
{
	//default values
	m_local_name = "/robotPathPlanner";
	
#if 1
    yDebug() << "config configuration: \n" << config.toString().c_str();

    std::string context_name = " robotPathPlanner";
    std::string file_name = " robotPathPlanner_cer.ini";

    if (config.check("context"))   context_name = config.find("context").asString();
    if (config.check("from")) file_name = config.find("from").asString();

    yarp::os::ResourceFinder rf;
    rf.setVerbose(true);
    rf.setDefaultContext(context_name.c_str());
    rf.setDefaultConfigFile(file_name.c_str());

    Property p;
    std::string configFile = rf.findFile("from");
    if (configFile != "") p.fromConfigFile(configFile.c_str());
    yDebug() << "robotPathPlannerDev configuration: \n" << p.toString().c_str();
    
    Bottle general_group = p.findGroup("GENERAL");
    if (general_group.isNull())
    {
        yError() << "Missing GENERAL group!";
        return false;
    }
    if (general_group.check("name")) m_local_name = general_group.find("name").asString();
    
#else
    Property p;
    p.fromString(config.toString());
#endif

    m_plannerThread = new PlannerThread(0.020,p);

    bool ret = m_rpcPort.open(m_local_name+"/rpc");
    if (ret == false)
    {
        yError() << "Unable to open module ports";
        return false;
    }

    m_rpcPort.setReader(*this);

    if (!m_plannerThread->start())
    {
        delete m_plannerThread;
        return false;
    }

    return true;
}

bool robotPathPlannerDev::close()
{
    m_rpcPort.interrupt();
    m_rpcPort.close();

    //gotoThread->shutdown();
    m_plannerThread->stop();
    delete m_plannerThread;
    m_plannerThread=NULL;

    return true;
}


bool robotPathPlannerDev::read(yarp::os::ConnectionReader& connection)
{
    yDebug();
    yarp::os::Bottle command;
    yarp::os::Bottle reply;
    bool ok = command.read(connection);
    if (!ok) return false;
    reply.clear();

    m_plannerThread->m_mutex.wait();
    if (command.get(0).isString())
    {
        if (command.get(0).asString()=="help")
        {
            reply.addVocab(Vocab::encode("many"));
            reply.addString("set_robot_radius <size_m>");
            reply.addString("get_robot_radius");
        }
        else if (command.get(0).isString())
        {
            parse_respond_string(command, reply);
        }
    }
    else
    {
        yError() << "Invalid command type";
        reply.addVocab(VOCAB_ERR);
    }

    yarp::os::ConnectionWriter *returnToSender = connection.getWriter();
    if (returnToSender != nullptr)
    {
        reply.write(*returnToSender);
    }
    m_plannerThread->m_mutex.post();
    return true;
}


bool robotPathPlannerDev::gotoTargetByAbsoluteLocation(Map2DLocation loc)
{
    bool b = m_plannerThread->setNewAbsTarget(loc);
    return b;
}

bool robotPathPlannerDev::gotoTargetByRelativeLocation(double x, double y, double theta)
{
    yarp::sig::Vector v;
    v.push_back(x);
    v.push_back(y);
    v.push_back(theta);
    bool b = m_plannerThread->setNewRelTarget(v);
    return b;
}

bool robotPathPlannerDev::recomputeCurrentNavigationPath()
{
    if (m_plannerThread->getNavigationStatusAsInt() == navigation_status_idle)
    {
        yError() << "Unable to recompute path. Navigation task not assigned yet.";
        return false;
    }
    if (m_plannerThread->getNavigationStatusAsInt() == navigation_status_paused)
    {
        yError() << "Unable to recompute path. Navigation task is currently paused.";
        return false;
    }
    if (m_plannerThread->getNavigationStatusAsInt() == navigation_status_goal_reached)
    {
        yError() << "Unable to recompute path. Navigation Goal has been already reached.";
        return false;
    }
    if (m_plannerThread->getNavigationStatusAsInt() == navigation_status_thinking)
    {
        yError() << "Unable to recompute path. A navigation plan is already under computation.";
        return false;
    }

    Map2DLocation loc;
    bool b = true;
    b &= m_plannerThread->getCurrentAbsTarget(loc);
    //@@@ check timing here
    yarp::os::Time::delay(0.2);
    b &= m_plannerThread->stopMovement();
    //@@@ check timing here
    yarp::os::Time::delay(0.2);
    b &= m_plannerThread->setNewAbsTarget(loc);
    if (b==false)
    {
        yError() << "robotPathPlannerDev::recomputeCurrentNavigationPath(). An error occurred while performing the requested operation.";
        return false;
    }
    return true;
}

bool robotPathPlannerDev::gotoTargetByRelativeLocation(double x, double y)
{
    yarp::sig::Vector v;
    v.push_back(x);
    v.push_back(y);
    bool b = m_plannerThread->setNewRelTarget(v);
    return b;
}

bool robotPathPlannerDev::applyVelocityCommand(double x_vel, double y_vel, double theta_vel, double timeout)
{
    yWarning() << "applyVelocityCommand() currently not implemented";
    return true;
}

bool robotPathPlannerDev::getAbsoluteLocationOfCurrentTarget(Map2DLocation& target)
{
    m_plannerThread->getCurrentAbsTarget(target);
    return true;
}

bool robotPathPlannerDev::getRelativeLocationOfCurrentTarget(double& x, double& y, double& theta)
{
    Map2DLocation loc;
    m_plannerThread->getCurrentRelTarget(loc);
    x=loc.x;
    y=loc.y;
    theta=loc.theta;
    return true;
}

bool robotPathPlannerDev::getNavigationStatus(NavigationStatusEnum& status)
{
     int nav_status = m_plannerThread->getNavigationStatusAsInt();
     status = (NavigationStatusEnum)(nav_status);
     return true;
}

bool robotPathPlannerDev::stopNavigation()
{
     bool b = m_plannerThread->stopMovement();
     return true;
}

bool robotPathPlannerDev::suspendNavigation(double time)
{
     bool b = m_plannerThread->pauseMovement(time);
     return b;
}

bool robotPathPlannerDev::resumeNavigation()
{
     bool b = m_plannerThread->resumeMovement();
     return b;
}

bool robotPathPlannerDev::getAllNavigationWaypoints(Map2DPath& waypoints)
{
    bool b = m_plannerThread->getCurrentPath(waypoints);
    return b;
}

bool robotPathPlannerDev::getCurrentNavigationWaypoint(Map2DLocation& curr_waypoint)
{
    bool b = m_plannerThread->getCurrentWaypoint(curr_waypoint);
    return b;
}

bool robotPathPlannerDev::getCurrentNavigationMap(NavigationMapTypeEnum map_type, MapGrid2D& map)
{
    if (map_type == NavigationMapTypeEnum::global_map)
    {
        m_plannerThread->getCurrentMap(map);
        return true;
    }
    else if (map_type == NavigationMapTypeEnum::local_map)
    {
        m_plannerThread->getOstaclesMap(map);
        return true;
    }
    return false;
}

bool robotPathPlannerDev::parse_respond_string(const yarp::os::Bottle& command, yarp::os::Bottle& reply)
{
    if (command.get(0).asString() == "set_robot_radius")
    {
        bool ret = this->m_plannerThread->setRobotRadius(command.get(1).asFloat64());
        if (ret)
        {
            reply.addString("set_robot_radius done");
        }
        else
        {
            reply.addString("set_robot_radius failed");
        }
    }
    if (command.get(0).asString() == "get_robot_radius")
    {
        double value = 0;
        bool ret = this->m_plannerThread->getRobotRadius(value);
        if (ret)
        {
            reply.addFloat64(value);
        }
        else
        {
            reply.addString("get_robot_radius failed");
        }
    }
    return true;
}
