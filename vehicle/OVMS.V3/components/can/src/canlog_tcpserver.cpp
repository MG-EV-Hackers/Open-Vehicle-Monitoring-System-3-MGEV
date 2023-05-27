/*
;    Project:       Open Vehicle Monitor System
;    Module:        CAN logging framework
;    Date:          18th January 2018
;
;    (C) 2018       Michael Balzer
;
; Permission is hereby granted, free of charge, to any person obtaining a copy
; of this software and associated documentation files (the "Software"), to deal
; in the Software without restriction, including without limitation the rights
; to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
; copies of the Software, and to permit persons to whom the Software is
; furnished to do so, subject to the following conditions:
;
; The above copyright notice and this permission notice shall be included in
; all copies or substantial portions of the Software.
;
; THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
; IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
; FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
; AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
; LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
; OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
; THE SOFTWARE.
*/

#include <sdkconfig.h>
#ifdef CONFIG_OVMS_SC_GPL_MONGOOSE

#include "ovms_log.h"
static const char *TAG = "canlog-tcpserver";

#include "can.h"
#include "canformat.h"
#include "canlog_tcpserver.h"
#include "ovms_config.h"
#include "ovms_peripherals.h"

canlog_tcpserver* MyCanLogTcpServer = NULL;

void can_log_tcpserver_start(int verbosity, OvmsWriter* writer, OvmsCommand* cmd, int argc, const char* const* argv)
  {
  std::string format(cmd->GetName());
  std::string mode(cmd->GetParent()->GetName());
  canlog_tcpserver* logger = new canlog_tcpserver(argv[0],format,GetFormatModeType(mode));

  if (logger->Open())
    {
    if (argc>1)
      { MyCan.AddLogger(logger, argc-1, &argv[1]); }
    else
      { MyCan.AddLogger(logger); }
    writer->printf("CAN logging as TCP server: %s\n", logger->GetInfo().c_str());
    }
  else
    {
    writer->printf("Error: Could not start CAN logging as TCP server: %s\n",logger->GetInfo().c_str());
    delete logger;
    }
  }

class OvmsCanLogTcpServerInit
  {
  public:
    OvmsCanLogTcpServerInit();
    void NetManInit(std::string event, void* data);
    void NetManStop(std::string event, void* data);
  } MyOvmsCanLogTcpServerInit  __attribute__ ((init_priority (4560)));

OvmsCanLogTcpServerInit::OvmsCanLogTcpServerInit()
  {
  ESP_LOGI(TAG, "Initialising CAN logging as TCP server (4560)");

  OvmsCommand* cmd_can = MyCommandApp.FindCommand("can");
  if (cmd_can)
    {
    OvmsCommand* cmd_can_log = cmd_can->FindCommand("log");
    if (cmd_can_log)
      {
      OvmsCommand* cmd_can_log_start = cmd_can_log->FindCommand("start");
      if (cmd_can_log_start)
        {
        // We have a place to put our command tree..
        OvmsCommand* start = cmd_can_log_start->RegisterCommand("tcpserver", "CAN logging as TCP server");
        OvmsCommand* discard = start->RegisterCommand("discard","CAN logging as TCP server (discard mode)");
        OvmsCommand* simulate = start->RegisterCommand("simulate","CAN logging as TCP server (simulate mode)");
        OvmsCommand* transmit = start->RegisterCommand("transmit","CAN logging as TCP server (transmit mode)");
        MyCanFormatFactory.RegisterCommandSet(discard, "Start CAN logging as TCP server (discard mode)",
          can_log_tcpserver_start,
          "<host[:port]> [filter1] ... [filterN]\n"
          "Filter: <bus> | <id>[-<id>] | <bus>:<id>[-<id>]\n"
          "Example: 2:2a0-37f",
          1, 9);
        MyCanFormatFactory.RegisterCommandSet(simulate, "Start CAN logging as TCP server (simulate mode)",
          can_log_tcpserver_start,
          "<host[:port]> [filter1] ... [filterN]\n"
          "Filter: <bus> | <id>[-<id>] | <bus>:<id>[-<id>]\n"
          "Example: 2:2a0-37f",
          1, 9);
        MyCanFormatFactory.RegisterCommandSet(transmit, "Start CAN logging as TCP server (transmit mode)",
          can_log_tcpserver_start,
          "<host[:port]> [filter1] ... [filterN]\n"
          "Filter: <bus> | <id>[-<id>] | <bus>:<id>[-<id>]\n"
          "Example: 2:2a0-37f",
          1, 9);
        }
      }
    }

  using std::placeholders::_1;
  using std::placeholders::_2;
  MyEvents.RegisterEvent(TAG, "network.mgr.init", std::bind(&OvmsCanLogTcpServerInit::NetManInit, this, _1, _2));
  MyEvents.RegisterEvent(TAG, "network.mgr.stop", std::bind(&OvmsCanLogTcpServerInit::NetManStop, this, _1, _2));
  }

void OvmsCanLogTcpServerInit::NetManInit(std::string event, void* data)
  {
  if (MyCanLogTcpServer) MyCanLogTcpServer->Open();
  }

void OvmsCanLogTcpServerInit::NetManStop(std::string event, void* data)
  {
  if (MyCanLogTcpServer) MyCanLogTcpServer->Close();
  }

#if MG_VERSION_NUMBER >= MG_VERSION_VAL(7, 0, 0)
static void tsMongooseHandler(struct mg_connection *nc, int ev, void *p, void *fn_data)
#else /* MG_VERSION_NUMBER */
static void tsMongooseHandler(struct mg_connection *nc, int ev, void *p)
#endif /* MG_VERSION_NUMBER */
  {
  if (MyCanLogTcpServer)
    MyCanLogTcpServer->MongooseHandler(nc, ev, p);
  else if (ev == MG_EV_ACCEPT)
    {
    ESP_LOGI(TAG, "Log service connection rejected (logger not running)");
#if MG_VERSION_NUMBER >= MG_VERSION_VAL(7, 0, 0)
    nc->is_closing = 1;
#else /* MG_VERSION_NUMBER */
    nc->flags |= MG_F_CLOSE_IMMEDIATELY;
#endif /* MG_VERSION_NUMBER */
    }
  }

canlog_tcpserver::canlog_tcpserver(std::string path, std::string format, canformat::canformat_serve_mode_t mode)
  : canlog("tcpserver", format, mode)
  {
  MyCanLogTcpServer = this;
  m_path = path;
  if (m_path.find(':') == std::string::npos)
    {
    m_path.append(":3000");
    }
  m_isopen = false;
  m_mgconn = NULL;
  }

canlog_tcpserver::~canlog_tcpserver()
  {
  Close();
  MyCanLogTcpServer = NULL;
  }

bool canlog_tcpserver::Open()
  {
  if (m_isopen) return true;

  ESP_LOGI(TAG, "Launching TCP server at %s",m_path.c_str());
  struct mg_mgr* mgr = MyNetManager.GetMongooseMgr();
  if (mgr != NULL)
    {
    if (MyNetManager.m_network_any)
      {
#if MG_VERSION_NUMBER >= MG_VERSION_VAL(7, 0, 0)
      m_mgconn = mg_listen(mgr, m_path.c_str(), tsMongooseHandler, NULL);
#else /* MG_VERSION_NUMBER */
      m_mgconn = mg_bind(mgr, m_path.c_str(), tsMongooseHandler);
#endif /* MG_VERSION_NUMBER */
      if (m_mgconn != NULL)
        {
        m_isopen = true;
        return true;
        }
      else
        {
        ESP_LOGE(TAG,"Could not listen on %s",m_path.c_str());
        return false;
        }
      }
    else
      {
      ESP_LOGI(TAG,"Delay TCP server (as network manager not up)");
      return true;
      }
    }
  else
    {
    ESP_LOGE(TAG,"Network manager is not available");
    return false;
    }
  }

void canlog_tcpserver::Close()
  {
  if (m_isopen)
    {
    if (m_connmap.size() > 0)
      {
      OvmsRecMutexLock lock(&m_cmmutex);
      for (conn_map_t::iterator it=m_connmap.begin(); it!=m_connmap.end(); ++it)
        {
#if MG_VERSION_NUMBER >= MG_VERSION_VAL(7, 0, 0)
        it->first->is_closing = 1;
#else /* MG_VERSION_NUMBER */
        it->first->flags |= MG_F_CLOSE_IMMEDIATELY;
#endif /* MG_VERSION_NUMBER */
        delete it->second;
        }
      m_connmap.clear();
      }
    ESP_LOGI(TAG, "Closed TCP server log: %s", GetStats().c_str());
#if MG_VERSION_NUMBER >= MG_VERSION_VAL(7, 0, 0)
    m_mgconn->is_closing = 1;
#else /* MG_VERSION_NUMBER */
    m_mgconn->flags |= MG_F_CLOSE_IMMEDIATELY;
#endif /* MG_VERSION_NUMBER */
    m_mgconn = NULL;
    m_isopen = false;
    }
  }

std::string canlog_tcpserver::GetInfo()
  {
  std::string result = canlog::GetInfo();
  result.append(" Path:");
  result.append(m_path);
  return result;
  }

void canlog_tcpserver::MongooseHandler(struct mg_connection *nc, int ev, void *p)
  {
  char addr[32];

  switch (ev)
    {
    case MG_EV_ACCEPT:
      {
      // New network connection has arrived
      OvmsRecMutexLock lock(&m_cmmutex);
#if MG_VERSION_NUMBER >= MG_VERSION_VAL(7, 10, 0)
      mg_snprintf(addr, sizeof(addr), "%M", mg_print_ip, &nc->rem);
#elif MG_VERSION_NUMBER >= MG_VERSION_VAL(7, 0, 0)
      mg_snprintf(addr, sizeof(addr), "%I", nc->rem.is_ip6 ? 16 : 4, nc->rem.is_ip6 ? &nc->rem.ip6 : (void *) &nc->rem.ip);
#else /* MG_VERSION_NUMBER */
      mg_sock_addr_to_str(&nc->sa, addr, sizeof(addr), MG_SOCK_STRINGIFY_IP);
#endif /* MG_VERSION_NUMBER */
      ESP_LOGI(TAG, "Log service connection from %s",addr);
      canlogconnection* clc = new canlogconnection(this, m_format, m_mode);
      clc->m_nc = nc;
      clc->m_peer = std::string(addr);
      m_connmap[nc] = clc;
      std::string result = clc->m_formatter->getheader();
      if (result.length()>0)
        {
        mg_send(nc, (const char*)result.c_str(), result.length());
        }
      break;
      }

    case MG_EV_CLOSE:
      {
      // Network connection has gone
      OvmsRecMutexLock lock(&m_cmmutex);
      auto k = m_connmap.find(nc);
      if (k != m_connmap.end())
        {
#if MG_VERSION_NUMBER >= MG_VERSION_VAL(7, 10, 0)
        mg_snprintf(addr, sizeof(addr), "%M", mg_print_ip, &nc->rem);
#elif MG_VERSION_NUMBER >= MG_VERSION_VAL(7, 0, 0)
        mg_snprintf(addr, sizeof(addr), "%I", nc->rem.is_ip6 ? 16 : 4, nc->rem.is_ip6 ? &nc->rem.ip6 : (void *) &nc->rem.ip);
#else /* MG_VERSION_NUMBER */
        mg_sock_addr_to_str(&nc->sa, addr, sizeof(addr), MG_SOCK_STRINGIFY_IP);
#endif /* MG_VERSION_NUMBER */
        ESP_LOGI(TAG, "Log service disconnection from %s",addr);
        delete k->second;
        m_connmap.erase(k);
        }
      break;
      }

#if MG_VERSION_NUMBER >= MG_VERSION_VAL(7, 0, 0)
    case MG_EV_READ:
      {
      // Receive data on the network connection
      size_t used = nc->recv.len;
#else /* MG_VERSION_NUMBER */
    case MG_EV_RECV:
      {
      // Receive data on the network connection
      size_t used = nc->recv_mbuf.len;
#endif /* MG_VERSION_NUMBER */
      //ESP_LOGD(TAG,"Received %d bytes of data",used);
      if (m_formatter != NULL)
        {
        canlogconnection* clc = NULL;
        auto k = m_connmap.find(nc);
        if (k != m_connmap.end()) clc = k->second;
#if MG_VERSION_NUMBER >= MG_VERSION_VAL(7, 0, 0)
        used = clc->m_formatter->Serve((uint8_t*)nc->recv.buf, used, clc);
#else /* MG_VERSION_NUMBER */
        used = clc->m_formatter->Serve((uint8_t*)nc->recv_mbuf.buf, used, clc);
#endif /* MG_VERSION_NUMBER */
        }
      if (used > 0)
        {
#if MG_VERSION_NUMBER >= MG_VERSION_VAL(7, 0, 0)
        mg_iobuf_del(&nc->recv, 0, used); // Removes `used` bytes from the beginning of the buffer.
#else /* MG_VERSION_NUMBER */
        mbuf_remove(&nc->recv_mbuf, used);
#endif /* MG_VERSION_NUMBER */
        }
      break;
      }

    default:
      break;
    }
  }

#endif // #ifdef CONFIG_OVMS_SC_GPL_MONGOOSE
