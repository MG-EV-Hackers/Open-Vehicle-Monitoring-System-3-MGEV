/*
;    Project:       Open Vehicle Monitor System
;    Date:          9th March 2020
;
;    Changes:
;    1.0  Initial release
;
;    (C) 2011       Michael Stegen / Stegen Electronics
;    (C) 2011-2017  Mark Webb-Johnson
;    (C) 2011        Sonny Chen @ EPRO/DX
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

#include "ovms_log.h"
static const char *TAG = "ovms-net";

#include "ovms.h"
#include "ovms_netconns.h"

////////////////////////////////////////////////////////////////////////////////
// OvmsMongooseWrapper
////////////////////////////////////////////////////////////////////////////////

#if MG_VERSION_NUMBER >= MG_VERSION_VAL(7, 0, 0)
static void OvmsMongooseWrapperCallback(struct mg_connection *nc, int ev, void *ev_data, void *fn_data)
#else /* MG_VERSION_NUMBER */
static void OvmsMongooseWrapperCallback(struct mg_connection *nc, int ev, void *ev_data)
#endif /* MG_VERSION_NUMBER */
  {
#if MG_VERSION_NUMBER >= MG_VERSION_VAL(7, 0, 0)
  OvmsMongooseWrapper* me = (OvmsMongooseWrapper*)nc->fn_data;
#else /* MG_VERSION_NUMBER */
  OvmsMongooseWrapper* me = (OvmsMongooseWrapper*)nc->user_data;
#endif /* MG_VERSION_NUMBER */

  if (me != NULL) me->Mongoose(nc, ev, ev_data);
  }

OvmsMongooseWrapper::OvmsMongooseWrapper()
  {
  }

OvmsMongooseWrapper::~OvmsMongooseWrapper()
  {
  }

void OvmsMongooseWrapper::Mongoose(struct mg_connection *nc, int ev, void *ev_data)
  {
  return;
  }

////////////////////////////////////////////////////////////////////////////////
// OvmsNetTcpClient
////////////////////////////////////////////////////////////////////////////////

OvmsNetTcpClient::OvmsNetTcpClient()
  {
  m_mgconn = NULL;
  m_netstate = NetConnIdle;
  }

OvmsNetTcpClient::~OvmsNetTcpClient()
  {
  if (m_mgconn)
    {
#if MG_VERSION_NUMBER >= MG_VERSION_VAL(7, 0, 0)
    m_mgconn->fn_data = NULL;
    m_mgconn->is_closing = 1;         // Tell mongoose to close this connection
#else /* MG_VERSION_NUMBER */
    m_mgconn->user_data = NULL;
    m_mgconn->flags |= MG_F_CLOSE_IMMEDIATELY;
#endif /* MG_VERSION_NUMBER */
    m_mgconn = NULL;
    m_netstate = NetConnIdle;
    }
  }

void OvmsNetTcpClient::Mongoose(struct mg_connection *nc, int ev, void *ev_data)
  {
  switch (ev)
    {
#if MG_VERSION_NUMBER >= MG_VERSION_VAL(7, 0, 0)
    case MG_EV_CONNECT:  // Connection established       NULL
      {
      // Successful connection
      ESP_LOGD(TAG, "OvmsNetTcpClient Connection successful");
      m_netstate = NetConnConnected;
      Connected();
      }
      break;
    case MG_EV_OPEN:
      {
      // Connection created. Store connect expiration time in c->data
      if (m_timeout > 0.0)
      {
        *(uint64_t *) nc->data = (uint64_t)(mg_millis() + m_timeout);
      } else {
        *(uint64_t *) nc->data = NULL;
      }
      break;
      }
    case MG_EV_POLL:
      {
      if (*(uint64_t *) nc->data != NULL)
      {
        if ((mg_millis() > *(uint64_t *) nc->data) &&
            (nc->is_connecting || nc->is_resolving))
          {
          ESP_LOGD(TAG, "OvmsNetTcpClient Connection timeout");
          m_mgconn->fn_data = NULL;
          mg_error(nc, "Connection timeout"); // will request a connection closure and fire the MG_EV_ERROR event.
          }
      }
      break;
      }
    case MG_EV_ERROR:
      {
      char * err = (char *) ev_data;
      ESP_LOGD(TAG, "OvmsNetTcpClient Connection failed: %s", err);
      m_netstate = NetConnFailed;
      m_mgconn = NULL;
      ConnectionFailed();
      break;
      }
#else /* MG_VERSION_NUMBER */
    case MG_EV_CONNECT:
      {
      mg_set_timer(m_mgconn, 0);
      int *success = (int*)ev_data;
      if (*success == 0)
        {
        // Successful connection
        ESP_LOGD(TAG, "OvmsNetTcpClient Connection successful");
        m_netstate = NetConnConnected;
        Connected();
        }
      else
        {
        // Connection failed
        ESP_LOGD(TAG, "OvmsNetTcpClient Connection failed");
        m_netstate = NetConnFailed;
        m_mgconn = NULL;
        ConnectionFailed();
        }
      }
      break;
    case MG_EV_TIMER:
      ESP_LOGD(TAG, "OvmsNetTcpClient Connection timeout");
      m_mgconn->user_data = NULL;
      m_mgconn->flags |= MG_F_CLOSE_IMMEDIATELY;
      m_mgconn = NULL;
      m_netstate = NetConnFailed;
      ConnectionFailed();
      break;
#endif /* MG_VERSION_NUMBER */
    case MG_EV_CLOSE:
      ESP_LOGD(TAG, "OvmsNetTcpClient Connection closed");
      m_netstate = NetConnDisconnected;
      m_mgconn = NULL;
      ConnectionClosed();
      break;
#if MG_VERSION_NUMBER >= MG_VERSION_VAL(7, 0, 0)
    case MG_EV_READ:
      {
      size_t removed = IncomingData(nc->recv.buf, nc->recv.len);
#else /* MG_VERSION_NUMBER */
    case MG_EV_RECV:
      {
      size_t removed = IncomingData(nc->recv_mbuf.buf, nc->recv_mbuf.len);
#endif /* MG_VERSION_NUMBER */
      if (removed > 0)
        {
        OvmsMutexLock mg(&m_mgconn_mutex);
#if MG_VERSION_NUMBER >= MG_VERSION_VAL(7, 0, 0)
        mg_iobuf_del(&nc->recv, 0, removed); // Removes `removed` bytes from the beginning of the buffer.
#else /* MG_VERSION_NUMBER */
        mbuf_remove(&nc->recv_mbuf, removed);
#endif /* MG_VERSION_NUMBER */
        }
      }
      break;
    default:
      break;
    }
  }

#if MG_VERSION_NUMBER >= MG_VERSION_VAL(7, 0, 0)
bool OvmsNetTcpClient::Connect(std::string dest, struct mg_tls_opts opts, double timeout)
#else /* MG_VERSION_NUMBER */
bool OvmsNetTcpClient::Connect(std::string dest, struct mg_connect_opts opts, double timeout)
#endif /* MG_VERSION_NUMBER */
  {
  struct mg_mgr* mgr = MyNetManager.GetMongooseMgr();
  if (mgr == NULL) return false;

  OvmsMutexLock mg(&m_mgconn_mutex);
  m_dest = dest;
#if MG_VERSION_NUMBER >= MG_VERSION_VAL(7, 0, 0)
  m_timeout = timeout;
  if ((m_mgconn = mg_connect(mgr, dest.c_str(), OvmsMongooseWrapperCallback, (void *)this)) == NULL)
#else /* MG_VERSION_NUMBER */
  opts.user_data = this;
  if ((m_mgconn = mg_connect_opt(mgr, dest.c_str(), OvmsMongooseWrapperCallback, opts)) == NULL)
#endif /* MG_VERSION_NUMBER */
    {
    return false;
    }
  if (timeout > 0.0)
    {
#if MG_VERSION_NUMBER >= MG_VERSION_VAL(7, 0, 0)
    // timeout data
#else /* MG_VERSION_NUMBER */
    mg_set_timer(m_mgconn, mg_time() + timeout);
#endif /* MG_VERSION_NUMBER */
    }
  m_netstate = NetConnConnecting;
  return true;
  }

void OvmsNetTcpClient::Disconnect()
  {
  if (m_mgconn)
    {
#if MG_VERSION_NUMBER >= MG_VERSION_VAL(7, 0, 0)
    m_mgconn->fn_data = NULL;
    m_mgconn->is_closing = 1;         // Tell mongoose to close this connection
#else /* MG_VERSION_NUMBER */
    m_mgconn->user_data = NULL;
    m_mgconn->flags |= MG_F_CLOSE_IMMEDIATELY;
#endif /* MG_VERSION_NUMBER */
    m_mgconn = NULL;
    m_netstate = NetConnIdle;
    }
  }

bool OvmsNetTcpClient::IsConnected()
  {
  return (m_netstate == NetConnConnected);
  }

size_t OvmsNetTcpClient::SendData(uint8_t *data, size_t length)
  {
  ESP_LOGD(TAG, "OvmsNetTcpClient Send data (%d bytes)", length);
  if (m_mgconn != NULL)
    {
    OvmsMutexLock mg(&m_mgconn_mutex);
    mg_send(m_mgconn, data, length);
    return length;
    }
  else
    return 0;
  }

void OvmsNetTcpClient::Connected()
  {
  }

void OvmsNetTcpClient::ConnectionFailed()
  {
  }

void OvmsNetTcpClient::ConnectionClosed()
  {
  }

size_t OvmsNetTcpClient::IncomingData(void *data, size_t length)
  {
  return length;
  }
