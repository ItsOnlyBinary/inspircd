/*       +------------------------------------+
 *       | Inspire Internet Relay Chat Daemon |
 *       +------------------------------------+
 *
 *  InspIRCd: (C) 2002-2009 InspIRCd Development Team
 * See: http://wiki.inspircd.org/Credits
 *
 * This program is free but copyrighted software; see
 *            the file COPYING for details.
 *
 * ---------------------------------------------------
 */

#ifndef __SOCKETENGINE_PORTS__
#define __SOCKETENGINE_PORTS__

#ifndef __sun
# error You need Solaris 10 or later to make use of this code.
#endif

#include <vector>
#include <string>
#include <map>
#include "inspircd_config.h"
#include "inspircd.h"
#include "socketengine.h"
#include <port.h>

/** A specialisation of the SocketEngine class, designed to use solaris 10 I/O completion ports
 */
class PortsEngine : public SocketEngine
{
private:
	/** These are used by epoll() to hold socket events
	 */
	port_event_t* events;
public:
	/** Create a new PortsEngine
	 * @param Instance The creator of this object
	 */
	PortsEngine();
	/** Delete a PortsEngine
	 */
	virtual ~PortsEngine();
	virtual bool AddFd(EventHandler* eh, int event_mask);
	void OnSetEvent(EventHandler* eh, int old_event, int new_event);
	virtual bool DelFd(EventHandler* eh, bool force = false);
	virtual int DispatchEvents();
	virtual std::string GetName();
	virtual void WantWrite(EventHandler* eh);
};

/** Creates a SocketEngine
 */
class SocketEngineFactory
{
public:
	/** Create a new instance of SocketEngine based on PortsEngine
	 */
	SocketEngine* Create() { return new PortsEngine; }
};

#endif

