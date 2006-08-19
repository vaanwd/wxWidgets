/////////////////////////////////////////////////////////////////////////////
// Name:       src/common/socket.cpp
// Purpose:    Socket handler classes
// Authors:    Guilhem Lavaux, Guillermo Rodriguez Garcia
// Created:    April 1997
// Copyright:  (C) 1999-1997, Guilhem Lavaux
//             (C) 2000-1999, Guillermo Rodriguez Garcia
// RCS_ID:     $Id$
// License:    wxWindows licence
/////////////////////////////////////////////////////////////////////////////

// ==========================================================================
// Declarations
// ==========================================================================

// For compilers that support precompilation, includes "wx.h".
#include "wx/wxprec.h"

#ifdef __BORLANDC__
    #pragma hdrstop
#endif

#if wxUSE_SOCKETS

#include "wx/socket.h"
#include "wx/gaddr.h"

#ifndef WX_PRECOMP
    #include "wx/object.h"
    #include "wx/string.h"
    #include "wx/intl.h"
    #include "wx/log.h"
    #include "wx/event.h"
    #include "wx/app.h"
    #include "wx/utils.h"
    #include "wx/timer.h"
#endif

#include "wx/apptrait.h"
#include "wx/module.h"

#include "wx/sckaddr.h"
#include "wx/datetime.h"

// DLL options compatibility check:
#include "wx/build.h"
WX_CHECK_BUILD_OPTIONS("wxNet")

// --------------------------------------------------------------------------
// macros and constants
// --------------------------------------------------------------------------

// discard buffer
#define MAX_DISCARD_SIZE (10 * 1024)

// what to do within waits: we have 2 cases: from the main thread itself we
// have to call wxYield() to let the events (including the GUI events and the
// low-level (not wxWidgets) events from GSocket) be processed. From another
// thread it is enough to just call wxThread::Yield() which will give away the
// rest of our time slice: the explanation is that the events will be processed
// by the main thread anyhow, without calling wxYield(), but we don't want to
// eat the CPU time uselessly while sitting in the loop waiting for the data
#if wxUSE_THREADS
    #define PROCESS_EVENTS()        \
    {                               \
        if ( wxThread::IsMain() )   \
            wxYield();              \
        else                        \
            wxThread::Yield();      \
    }
#else // !wxUSE_THREADS
    #define PROCESS_EVENTS() wxYield()
#endif // wxUSE_THREADS/!wxUSE_THREADS

#define wxTRACE_Socket _T("wxSocket")

// --------------------------------------------------------------------------
// Helper functions
// --------------------------------------------------------------------------
    
void PokeUInt16(void* p, uint16_t value)
{
#if defined(__arm__) or defined(__sparc__)
  // Avoid aligment issues.
  memcpy(p, &value, sizeof(uint16_t));
#else
  *((uint16_t*)p) = value;
#endif  
}
    
void PokeUInt16_BE(void* p, uint16_t value)
{
  PokeUInt16(p,wxUINT16_SWAP_ON_LE(value));
}

void PokeUInt32(void* p, uint32_t value)
{
#if defined(__arm__) or defined(__sparc__)
  // Avoid aligment issues.
  memcpy(p, &value, sizeof(uint32_t));
#else
  *((uint32_t*)p) = value;
#endif  
}
    
void PokeUInt32_BE(void* p, uint32_t value)
{
  PokeUInt32(p,wxUINT32_SWAP_ON_LE(value));
}

// --------------------------------------------------------------------------
// wxWin macros
// --------------------------------------------------------------------------

IMPLEMENT_CLASS(wxSocketBase, wxObject)
IMPLEMENT_CLASS(wxSocketServer, wxSocketBase)
IMPLEMENT_CLASS(wxSocketClient, wxSocketBase)
IMPLEMENT_CLASS(wxDatagramSocket, wxSocketBase)
IMPLEMENT_DYNAMIC_CLASS(wxSocketEvent, wxEvent)

// --------------------------------------------------------------------------
// private classes
// --------------------------------------------------------------------------

class wxSocketState : public wxObject
{
public:
  wxSocketFlags            m_flags;
  wxSocketEventFlags       m_eventmask;
  bool                     m_notify;
  void                    *m_clientData;

public:
  wxSocketState() : wxObject() {}

  DECLARE_NO_COPY_CLASS(wxSocketState)
};

// ==========================================================================
// wxSocketBase
// ==========================================================================

// --------------------------------------------------------------------------
// Initialization and shutdown
// --------------------------------------------------------------------------

// FIXME-MT: all this is MT-unsafe, of course, we should protect all accesses
//           to m_countInit with a crit section
size_t wxSocketBase::m_countInit = 0;

bool wxSocketBase::IsInitialized()
{
    return m_countInit > 0;
}

bool wxSocketBase::Initialize()
{
    if ( !m_countInit++ )
    {
        /*
            Details: Initialize() creates a hidden window as a sink for socket
            events, such as 'read completed'. wxMSW has only one message loop
            for the main thread. If Initialize is called in a secondary thread,
            the socket window will be created for the secondary thread, but
            since there is no message loop on this thread, it will never
            receive events and all socket operations will time out.
            BTW, the main thread must not be stopped using sleep or block
            on a semaphore (a bad idea in any case) or socket operations
            will time out.

            On the Mac side, Initialize() stores a pointer to the CFRunLoop for
            the main thread. Because secondary threads do not have run loops,
            adding event notifications to the "Current" loop would have no
            effect at all, events would never fire.
        */
        wxASSERT_MSG( wxIsMainThread(),
            wxT("Call wxSocketBase::Initialize() from the main thread first!"));

        wxAppTraits *traits = wxAppConsole::GetInstance() ?
                              wxAppConsole::GetInstance()->GetTraits() : NULL;
        GSocketGUIFunctionsTable *functions =
            traits ? traits->GetSocketGUIFunctionsTable() : NULL;
        GSocket_SetGUIFunctions(functions);

        if ( !GSocket_Init() )
        {
            m_countInit--;

            return false;
        }
    }

    return true;
}

void wxSocketBase::Shutdown()
{
    // we should be initialized
    wxASSERT_MSG( m_countInit, _T("extra call to Shutdown()") );
    if ( --m_countInit == 0 )
    {
        GSocket_Cleanup();
    }
}

// --------------------------------------------------------------------------
// Ctor and dtor
// --------------------------------------------------------------------------

void wxSocketBase::Init()
{
  m_socket       = NULL;
  m_type         = wxSOCKET_UNINIT;

  // state
  m_flags        = 0;
  m_connected    =
  m_establishing =
  m_reading      =
  m_writing      =
  m_error        = false;
  m_lcount       = 0;
  m_timeout      = 600;
  m_beingDeleted = false;

  // pushback buffer
  m_unread       = NULL;
  m_unrd_size    = 0;
  m_unrd_cur     = 0;

  // events
  m_id           = wxID_ANY;
  m_handler      = NULL;
  m_clientData   = NULL;
  m_notify       = false;
  m_eventmask    = 0;

  if ( !IsInitialized() )
  {
      // this Initialize() will be undone by wxSocketModule::OnExit(), all the
      // other calls to it should be matched by a call to Shutdown()
      Initialize();
  }
}

wxSocketBase::wxSocketBase()
{
  Init();
}

wxSocketBase::wxSocketBase(wxSocketFlags flags, wxSocketType type)
{
  Init();

  m_flags = flags;
  m_type  = type;
}

wxSocketBase::~wxSocketBase()
{
  // Just in case the app called Destroy() *and* then deleted
  // the socket immediately: don't leave dangling pointers.
  wxAppTraits *traits = wxTheApp ? wxTheApp->GetTraits() : NULL;
  if ( traits )
      traits->RemoveFromPendingDelete(this);

  // Shutdown and close the socket
  if (!m_beingDeleted)
    Close();

  // Destroy the GSocket object
  if (m_socket)
    delete m_socket;

  // Free the pushback buffer
  if (m_unread)
    free(m_unread);
}

bool wxSocketBase::Destroy()
{
  // Delayed destruction: the socket will be deleted during the next
  // idle loop iteration. This ensures that all pending events have
  // been processed.
  m_beingDeleted = true;

  // Shutdown and close the socket
  Close();

  // Supress events from now on
  Notify(false);

  // schedule this object for deletion
  wxAppTraits *traits = wxTheApp ? wxTheApp->GetTraits() : NULL;
  if ( traits )
  {
      // let the traits object decide what to do with us
      traits->ScheduleForDestroy(this);
  }
  else // no app or no traits
  {
      // in wxBase we might have no app object at all, don't leak memory
      delete this;
  }

  return true;
}

// --------------------------------------------------------------------------
// Basic IO calls
// --------------------------------------------------------------------------

// The following IO operations update m_error and m_lcount:
// {Read, Write, ReadMsg, WriteMsg, Peek, Unread, Discard}
//
// TODO: Should Connect, Accept and AcceptWith update m_error?

bool wxSocketBase::Close()
{
  // Interrupt pending waits
  InterruptWait();

  if (m_socket)
  {
    // Disable callbacks
    m_socket->UnsetCallback(GSOCK_INPUT_FLAG | GSOCK_OUTPUT_FLAG |
                                    GSOCK_LOST_FLAG | GSOCK_CONNECTION_FLAG);

    // Shutdown the connection
    m_socket->Shutdown();
  }

  m_connected = false;
  m_establishing = false;
  return true;
}

wxSocketBase& wxSocketBase::Read(void* buffer, wxUint32 nbytes)
{
  // Mask read events
  m_reading = true;
  m_lcount = _Read(buffer, nbytes);

  // If in wxSOCKET_WAITALL mode, all bytes should have been read.
  if (m_flags & wxSOCKET_WAITALL)
    m_error = (m_lcount != nbytes);
  else
    m_error = (m_lcount == 0);

  // Allow read events from now on
  m_reading = false;

  return *this;
}

wxUint32 wxSocketBase::_Read(void* buffer, wxUint32 nbytes)
{
  int total;

  // Try the pushback buffer first
  total = GetPushback(buffer, nbytes, false);
  nbytes -= total;
  buffer  = (char *)buffer + total;

  // Return now in one of the following cases:
  // - the socket is invalid,
  // - we got all the data
  if ( !m_socket ||
       !nbytes )
    return total;

  // Possible combinations (they are checked in this order)
  // wxSOCKET_NOWAIT
  // wxSOCKET_WAITALL (with or without wxSOCKET_BLOCK)
  // wxSOCKET_BLOCK
  // wxSOCKET_NONE
  //
  int ret;
  if (m_flags & wxSOCKET_NOWAIT)
  {
    m_socket->SetNonBlocking(1);
    ret = m_socket->Read((char *)buffer, nbytes);
    m_socket->SetNonBlocking(0);

    if (ret > 0)
      total += ret;
  }
  else
  {
    bool more = true;

    while (more)
    {
      if ( !(m_flags & wxSOCKET_BLOCK) && !WaitForRead() )
        break;

      ret = m_socket->Read((char *)buffer, nbytes);

      if (ret > 0)
      {
        total  += ret;
        nbytes -= ret;
        buffer  = (char *)buffer + ret;
      }

      // If we got here and wxSOCKET_WAITALL is not set, we can leave
      // now. Otherwise, wait until we recv all the data or until there
      // is an error.
      //
      more = (ret > 0 && nbytes > 0 && (m_flags & wxSOCKET_WAITALL));
    }
  }

  return total;
}

wxSocketBase& wxSocketBase::ReadMsg(void* buffer, wxUint32 nbytes)
{
  wxUint32 len, len2, sig, total;
  bool error;
  int old_flags;
  struct
  {
    unsigned char sig[4];
    unsigned char len[4];
  } msg;

  // Mask read events
  m_reading = true;

  total = 0;
  error = true;
  old_flags = m_flags;
  SetFlags((m_flags & wxSOCKET_BLOCK) | wxSOCKET_WAITALL);

  if (_Read(&msg, sizeof(msg)) != sizeof(msg))
    goto exit;

  sig = (wxUint32)msg.sig[0];
  sig |= (wxUint32)(msg.sig[1] << 8);
  sig |= (wxUint32)(msg.sig[2] << 16);
  sig |= (wxUint32)(msg.sig[3] << 24);

  if (sig != 0xfeeddead)
  {
    wxLogWarning(_("wxSocket: invalid signature in ReadMsg."));
    goto exit;
  }

  len = (wxUint32)msg.len[0];
  len |= (wxUint32)(msg.len[1] << 8);
  len |= (wxUint32)(msg.len[2] << 16);
  len |= (wxUint32)(msg.len[3] << 24);

  if (len > nbytes)
  {
    len2 = len - nbytes;
    len = nbytes;
  }
  else
    len2 = 0;

  // Don't attemp to read if the msg was zero bytes long.
  if (len)
  {
    total = _Read(buffer, len);

    if (total != len)
      goto exit;
  }
  if (len2)
  {
    char *discard_buffer = new char[MAX_DISCARD_SIZE];
    long discard_len;

    // NOTE: discarded bytes don't add to m_lcount.
    do
    {
      discard_len = ((len2 > MAX_DISCARD_SIZE)? MAX_DISCARD_SIZE : len2);
      discard_len = _Read(discard_buffer, (wxUint32)discard_len);
      len2 -= (wxUint32)discard_len;
    }
    while ((discard_len > 0) && len2);

    delete [] discard_buffer;

    if (len2 != 0)
      goto exit;
  }
  if (_Read(&msg, sizeof(msg)) != sizeof(msg))
    goto exit;

  sig = (wxUint32)msg.sig[0];
  sig |= (wxUint32)(msg.sig[1] << 8);
  sig |= (wxUint32)(msg.sig[2] << 16);
  sig |= (wxUint32)(msg.sig[3] << 24);

  if (sig != 0xdeadfeed)
  {
    wxLogWarning(_("wxSocket: invalid signature in ReadMsg."));
    goto exit;
  }

  // everything was OK
  error = false;

exit:
  m_error = error;
  m_lcount = total;
  m_reading = false;
  SetFlags(old_flags);

  return *this;
}

wxSocketBase& wxSocketBase::Peek(void* buffer, wxUint32 nbytes)
{
  // Mask read events
  m_reading = true;

  m_lcount = _Read(buffer, nbytes);
  Pushback(buffer, m_lcount);

  // If in wxSOCKET_WAITALL mode, all bytes should have been read.
  if (m_flags & wxSOCKET_WAITALL)
    m_error = (m_lcount != nbytes);
  else
    m_error = (m_lcount == 0);

  // Allow read events again
  m_reading = false;

  return *this;
}

wxSocketBase& wxSocketBase::Write(const void *buffer, wxUint32 nbytes)
{
  // Mask write events
  m_writing = true;

  m_lcount = _Write(buffer, nbytes);

  // If in wxSOCKET_WAITALL mode, all bytes should have been written.
  if (m_flags & wxSOCKET_WAITALL)
    m_error = (m_lcount != nbytes);
  else
    m_error = (m_lcount == 0);

  // Allow write events again
  m_writing = false;

  return *this;
}

wxUint32 wxSocketBase::_Write(const void *buffer, wxUint32 nbytes)
{
  wxUint32 total = 0;

  // If the socket is invalid or parameters are ill, return immediately
  if (!m_socket || !buffer || !nbytes)
    return 0;

  // Possible combinations (they are checked in this order)
  // wxSOCKET_NOWAIT
  // wxSOCKET_WAITALL (with or without wxSOCKET_BLOCK)
  // wxSOCKET_BLOCK
  // wxSOCKET_NONE
  //
  int ret;
  if (m_flags & wxSOCKET_NOWAIT)
  {
    m_socket->SetNonBlocking(1);
    ret = m_socket->Write((const char *)buffer, nbytes);
    m_socket->SetNonBlocking(0);

    if (ret > 0)
      total = ret;
  }
  else
  {
    bool more = true;

    while (more)
    {
      if ( !(m_flags & wxSOCKET_BLOCK) && !WaitForWrite() )
        break;

      ret = m_socket->Write((const char *)buffer, nbytes);

      if (ret > 0)
      {
        total  += ret;
        nbytes -= ret;
        buffer  = (const char *)buffer + ret;
      }

      // If we got here and wxSOCKET_WAITALL is not set, we can leave
      // now. Otherwise, wait until we send all the data or until there
      // is an error.
      //
      more = (ret > 0 && nbytes > 0 && (m_flags & wxSOCKET_WAITALL));
    }
  }

  return total;
}

wxSocketBase& wxSocketBase::WriteMsg(const void *buffer, wxUint32 nbytes)
{
  wxUint32 total;
  bool error;
  struct
  {
    unsigned char sig[4];
    unsigned char len[4];
  } msg;

  // Mask write events
  m_writing = true;

  error = true;
  total = 0;
  SetFlags((m_flags & wxSOCKET_BLOCK) | wxSOCKET_WAITALL);

  msg.sig[0] = (unsigned char) 0xad;
  msg.sig[1] = (unsigned char) 0xde;
  msg.sig[2] = (unsigned char) 0xed;
  msg.sig[3] = (unsigned char) 0xfe;

  msg.len[0] = (unsigned char) (nbytes & 0xff);
  msg.len[1] = (unsigned char) ((nbytes >> 8) & 0xff);
  msg.len[2] = (unsigned char) ((nbytes >> 16) & 0xff);
  msg.len[3] = (unsigned char) ((nbytes >> 24) & 0xff);

  if (_Write(&msg, sizeof(msg)) < sizeof(msg))
    goto exit;

  total = _Write(buffer, nbytes);

  if (total < nbytes)
    goto exit;

  msg.sig[0] = (unsigned char) 0xed;
  msg.sig[1] = (unsigned char) 0xfe;
  msg.sig[2] = (unsigned char) 0xad;
  msg.sig[3] = (unsigned char) 0xde;
  msg.len[0] = msg.len[1] = msg.len[2] = msg.len[3] = (char) 0;

  if ((_Write(&msg, sizeof(msg))) < sizeof(msg))
    goto exit;

  // everything was OK
  error = false;

exit:
  m_error = error;
  m_lcount = total;
  m_writing = false;

  return *this;
}

wxSocketBase& wxSocketBase::Unread(const void *buffer, wxUint32 nbytes)
{
  if (nbytes != 0)
    Pushback(buffer, nbytes);

  m_error = false;
  m_lcount = nbytes;

  return *this;
}

wxSocketBase& wxSocketBase::Discard()
{
  char *buffer = new char[MAX_DISCARD_SIZE];
  wxUint32 ret;
  wxUint32 total = 0;

  // Mask read events
  m_reading = true;

  SetFlags(wxSOCKET_NOWAIT);

  do
  {
    ret = _Read(buffer, MAX_DISCARD_SIZE);
    total += ret;
  }
  while (ret == MAX_DISCARD_SIZE);

  delete[] buffer;
  m_lcount = total;
  m_error  = false;

  // Allow read events again
  m_reading = false;

  return *this;
}

// --------------------------------------------------------------------------
// Wait functions
// --------------------------------------------------------------------------

// All Wait functions poll the socket using GSocket_Select() to
// check for the specified combination of conditions, until one
// of these conditions become true, an error occurs, or the
// timeout elapses. The polling loop calls PROCESS_EVENTS(), so
// this won't block the GUI.

bool wxSocketBase::_Wait(long seconds,
                         long milliseconds,
                         wxSocketEventFlags flags)
{
  GSocketEventFlags result;
  long timeout;

  // Set this to true to interrupt ongoing waits
  m_interrupt = false;

  // Check for valid socket
  if (!m_socket)
    return false;

  // Check for valid timeout value.
  if (seconds != -1)
    timeout = seconds * 1000 + milliseconds;
  else
    timeout = m_timeout * 1000;

  bool has_event_loop = wxTheApp->GetTraits() ? (wxTheApp->GetTraits()->GetSocketGUIFunctionsTable() ? true : false) : false;

  // Wait in an active polling loop.
  //
  // NOTE: We duplicate some of the code in OnRequest, but this doesn't
  //   hurt. It has to be here because the (GSocket) event might arrive
  //   a bit delayed, and it has to be in OnRequest as well because we
  //   don't know whether the Wait functions are being used.
  //
  // Do this at least once (important if timeout == 0, when
  // we are just polling). Also, if just polling, do not yield.

  wxLongLong time_limit = wxDateTime::UNow().GetValue() + timeout;
  bool done = false;
  bool valid_result = false;

  if (!has_event_loop)
  {
    // This is used to avoid a busy loop on wxBase - having a select
    // timeout of 50 ms per iteration should be enough.
    if (timeout > 50)
      m_socket->SetTimeout(50);
    else
      m_socket->SetTimeout(timeout);
  }

  while (!done)
  {
    result = m_socket->Select(flags | GSOCK_LOST_FLAG);

    // Incoming connection (server) or connection established (client)
    if (result & GSOCK_CONNECTION_FLAG)
    {
      m_connected = true;
      m_establishing = false;
      valid_result = true;
      break;
    }

    // Data available or output buffer ready
    if ((result & GSOCK_INPUT_FLAG) || (result & GSOCK_OUTPUT_FLAG))
    {
      valid_result = true;
      break;
    }

    // Connection lost
    if (result & GSOCK_LOST_FLAG)
    {
      m_connected = false;
      m_establishing = false;
      valid_result = ((flags & GSOCK_LOST_FLAG) != 0);
      break;
    }

    // Wait more?
    wxLongLong time_left = time_limit - wxDateTime::UNow().GetValue();
    if ((!timeout) || (time_left <= 0) || (m_interrupt))
      done = true;
    else
    {
      if (has_event_loop)
      {
          PROCESS_EVENTS();
      }
      else
      {
        // If there's less than 50 ms left, just call select with that timeout.
        if (time_left < 50)
          m_socket->SetTimeout(time_left.ToLong());
      }
    }
  }

  // Set timeout back to original value (we overwrote it for polling)
  if (!has_event_loop)
    m_socket->SetTimeout(m_timeout*1000);

  return valid_result;
}

bool wxSocketBase::Wait(long seconds, long milliseconds)
{
    return _Wait(seconds, milliseconds, GSOCK_INPUT_FLAG |
                                        GSOCK_OUTPUT_FLAG |
                                        GSOCK_CONNECTION_FLAG |
                                        GSOCK_LOST_FLAG);
}

bool wxSocketBase::WaitForRead(long seconds, long milliseconds)
{
  // Check pushback buffer before entering _Wait
  if (m_unread)
    return true;

  // Note that GSOCK_INPUT_LOST has to be explicitly passed to
  // _Wait because of the semantics of WaitForRead: a return
  // value of true means that a GSocket_Read call will return
  // immediately, not that there is actually data to read.

  return _Wait(seconds, milliseconds, GSOCK_INPUT_FLAG |
                                      GSOCK_LOST_FLAG);
}


bool wxSocketBase::WaitForWrite(long seconds, long milliseconds)
{
    return _Wait(seconds, milliseconds, GSOCK_OUTPUT_FLAG);
}

bool wxSocketBase::WaitForLost(long seconds, long milliseconds)
{
    return _Wait(seconds, milliseconds, GSOCK_LOST_FLAG);
}

// --------------------------------------------------------------------------
// Miscellaneous
// --------------------------------------------------------------------------

//
// Get local or peer address
//

bool wxSocketBase::GetPeer(wxSockAddress& addr_man) const
{
  GAddress *peer;

  if (!m_socket)
    return false;

  peer = m_socket->GetPeer();

    // copying a null address would just trigger an assert anyway

  if (!peer)
    return false;

  addr_man.SetAddress(peer);
  GAddress_destroy(peer);

  return true;
}

bool wxSocketBase::GetLocal(wxSockAddress& addr_man) const
{
    GAddress *local;

    if (!m_socket)
        return false;

    local = m_socket->GetLocal();
    addr_man.SetAddress(local);
    GAddress_destroy(local);

    return true;
}

//
// Save and restore socket state
//

void wxSocketBase::SaveState()
{
    wxSocketState *state;

    state = new wxSocketState();

    state->m_flags      = m_flags;
    state->m_notify     = m_notify;
    state->m_eventmask  = m_eventmask;
    state->m_clientData = m_clientData;

    m_states.Append(state);
}

void wxSocketBase::RestoreState()
{
    wxList::compatibility_iterator node;
    wxSocketState *state;

    node = m_states.GetLast();
    if (!node)
        return;

    state = (wxSocketState *)node->GetData();

    m_flags      = state->m_flags;
    m_notify     = state->m_notify;
    m_eventmask  = state->m_eventmask;
    m_clientData = state->m_clientData;

    m_states.Erase(node);
    delete state;
}

//
// Timeout and flags
//

void wxSocketBase::SetTimeout(long seconds)
{
    m_timeout = seconds;

    if (m_socket)
        m_socket->SetTimeout(m_timeout * 1000);
}

void wxSocketBase::SetFlags(wxSocketFlags flags)
{
    m_flags = flags;
}


// --------------------------------------------------------------------------
// Event handling
// --------------------------------------------------------------------------

// A note on how events are processed, which is probably the most
// difficult thing to get working right while keeping the same API
// and functionality for all platforms.
//
// When GSocket detects an event, it calls wx_socket_callback, which in
// turn just calls wxSocketBase::OnRequest in the corresponding wxSocket
// object. OnRequest does some housekeeping, and if the event is to be
// propagated to the user, it creates a new wxSocketEvent object and
// posts it. The event is not processed immediately, but delayed with
// AddPendingEvent instead. This is necessary in order to decouple the
// event processing from wx_socket_callback; otherwise, subsequent IO
// calls made from the user event handler would fail, as gtk callbacks
// are not reentrant.
//
// Note that, unlike events, user callbacks (now deprecated) are _not_
// decoupled from wx_socket_callback and thus they suffer from a variety
// of problems. Avoid them where possible and use events instead.

extern "C"
void LINKAGEMODE wx_socket_callback(GSocket * WXUNUSED(socket),
                                    GSocketEvent notification,
                                    char *cdata)
{
    wxSocketBase *sckobj = (wxSocketBase *)cdata;

    sckobj->OnRequest((wxSocketNotify) notification);
}

void wxSocketBase::OnRequest(wxSocketNotify notification)
{
  // NOTE: We duplicate some of the code in _Wait, but this doesn't
  //   hurt. It has to be here because the (GSocket) event might arrive
  //   a bit delayed, and it has to be in _Wait as well because we don't
  //   know whether the Wait functions are being used.

  switch(notification)
  {
    case wxSOCKET_CONNECTION:
      m_establishing = false;
      m_connected = true;
      break;

    // If we are in the middle of a R/W operation, do not
    // propagate events to users. Also, filter 'late' events
    // which are no longer valid.

    case wxSOCKET_INPUT:
      if (m_reading || !m_socket->Select(GSOCK_INPUT_FLAG))
        return;
      break;

    case wxSOCKET_OUTPUT:
      if (m_writing || !m_socket->Select(GSOCK_OUTPUT_FLAG))
        return;
      break;

    case wxSOCKET_LOST:
      m_connected = false;
      m_establishing = false;
      break;

    default:
      break;
  }

  // Schedule the event

  wxSocketEventFlags flag = 0;
  wxUnusedVar(flag);
  switch (notification)
  {
    case GSOCK_INPUT:      flag = GSOCK_INPUT_FLAG; break;
    case GSOCK_OUTPUT:     flag = GSOCK_OUTPUT_FLAG; break;
    case GSOCK_CONNECTION: flag = GSOCK_CONNECTION_FLAG; break;
    case GSOCK_LOST:       flag = GSOCK_LOST_FLAG; break;
    default:
      wxLogWarning(_("wxSocket: unknown event!."));
      return;
  }

  if (((m_eventmask & flag) == flag) && m_notify)
  {
    if (m_handler)
    {
      wxSocketEvent event(m_id);
      event.m_event      = notification;
      event.m_clientData = m_clientData;
      event.SetEventObject(this);

      m_handler->AddPendingEvent(event);
    }
  }
}

void wxSocketBase::Notify(bool notify)
{
    m_notify = notify;
}

void wxSocketBase::SetNotify(wxSocketEventFlags flags)
{
    m_eventmask = flags;
}

void wxSocketBase::SetEventHandler(wxEvtHandler& handler, int id)
{
    m_handler = &handler;
    m_id      = id;
}

// --------------------------------------------------------------------------
// Pushback buffer
// --------------------------------------------------------------------------

void wxSocketBase::Pushback(const void *buffer, wxUint32 size)
{
  if (!size) return;

  if (m_unread == NULL)
    m_unread = malloc(size);
  else
  {
    void *tmp;

    tmp = malloc(m_unrd_size + size);
    memcpy((char *)tmp + size, m_unread, m_unrd_size);
    free(m_unread);

    m_unread = tmp;
  }

  m_unrd_size += size;

  memcpy(m_unread, buffer, size);
}

wxUint32 wxSocketBase::GetPushback(void *buffer, wxUint32 size, bool peek)
{
  if (!m_unrd_size)
    return 0;

  if (size > (m_unrd_size-m_unrd_cur))
    size = m_unrd_size-m_unrd_cur;

  memcpy(buffer, (char *)m_unread + m_unrd_cur, size);

  if (!peek)
  {
    m_unrd_cur += size;
    if (m_unrd_size == m_unrd_cur)
    {
      free(m_unread);
      m_unread = NULL;
      m_unrd_size = 0;
      m_unrd_cur  = 0;
    }
  }

  return size;
}


// ==========================================================================
// wxSocketServer
// ==========================================================================

// --------------------------------------------------------------------------
// Ctor
// --------------------------------------------------------------------------

wxSocketServer::wxSocketServer(const wxSockAddress& addr_man,
                               wxSocketFlags flags)
              : wxSocketBase(flags, wxSOCKET_SERVER)
{
    wxLogTrace( wxTRACE_Socket, _T("Opening wxSocketServer") );

    m_socket = GSocket_new();

    if (!m_socket)
    {
        wxLogTrace( wxTRACE_Socket, _T("*** GSocket_new failed") );
        return;
    }

        // Setup the socket as server

    m_socket->SetLocal(addr_man.GetAddress());

    if (GetFlags() & wxSOCKET_REUSEADDR) {
        m_socket->SetReusable();
    }

    if (m_socket->SetServer() != GSOCK_NOERROR)
    {
        delete m_socket;
        m_socket = NULL;

        wxLogTrace( wxTRACE_Socket, _T("*** GSocket_SetServer failed") );
        return;
    }

    m_socket->SetTimeout(m_timeout * 1000);
    m_socket->SetCallback(GSOCK_INPUT_FLAG | GSOCK_OUTPUT_FLAG |
                                  GSOCK_LOST_FLAG | GSOCK_CONNECTION_FLAG,
                                  wx_socket_callback, (char *)this);
}

// --------------------------------------------------------------------------
// Accept
// --------------------------------------------------------------------------

bool wxSocketServer::AcceptWith(wxSocketBase& sock, bool wait)
{
  GSocket *child_socket;

  if (!m_socket)
    return false;

  // If wait == false, then the call should be nonblocking.
  // When we are finished, we put the socket to blocking mode
  // again.

  if (!wait)
    m_socket->SetNonBlocking(1);

  child_socket = m_socket->WaitConnection();

  if (!wait)
    m_socket->SetNonBlocking(0);

  if (!child_socket)
    return false;

  sock.m_type = wxSOCKET_BASE;
  sock.m_socket = child_socket;
  sock.m_connected = true;

  sock.m_socket->SetTimeout(sock.m_timeout * 1000);
  sock.m_socket->SetCallback(GSOCK_INPUT_FLAG | GSOCK_OUTPUT_FLAG |
                                     GSOCK_LOST_FLAG | GSOCK_CONNECTION_FLAG,
                                     wx_socket_callback, (char *)&sock);

  return true;
}

wxSocketBase *wxSocketServer::Accept(bool wait)
{
  wxSocketBase* sock = new wxSocketBase();

  sock->SetFlags(m_flags);

  if (!AcceptWith(*sock, wait))
  {
    sock->Destroy();
    sock = NULL;
  }

  return sock;
}

bool wxSocketServer::WaitForAccept(long seconds, long milliseconds)
{
    return _Wait(seconds, milliseconds, GSOCK_CONNECTION_FLAG);
}

bool wxSocketBase::GetOption(int level, int optname, void *optval, int *optlen)
{
    wxASSERT_MSG( m_socket, _T("Socket not initialised") );

    if (m_socket->GetSockOpt(level, optname, optval, optlen)
        != GSOCK_NOERROR)
    {
        return false;
    }
    return true;
}

bool wxSocketBase::SetOption(int level, int optname, const void *optval,
                              int optlen)
{
    wxASSERT_MSG( m_socket, _T("Socket not initialised") );

    if (m_socket->SetSockOpt(level, optname, optval, optlen)
        != GSOCK_NOERROR)
    {
        return false;
    }
    return true;
}

bool wxSocketBase::SetLocal(wxIPV4address& local)
{
  GAddress* la = local.GetAddress();

  // If the address is valid, save it for use when we call Connect
  if (la && la->m_addr)
  {
    m_localAddress = local;

    return true;
  }

  return false;
}

// ==========================================================================
// wxSocketClient
// ==========================================================================

// --------------------------------------------------------------------------
// Ctor and dtor
// --------------------------------------------------------------------------

wxSocketClient::wxSocketClient(wxSocketFlags flags)
              : wxSocketBase(flags, wxSOCKET_CLIENT)
{
  m_proxy_type = wxPROXY_NONE;
}

wxSocketClient::~wxSocketClient()
{
}

// --------------------------------------------------------------------------
// Connect
// --------------------------------------------------------------------------

bool wxSocketClient::DoConnect(wxSockAddress& addr_man, wxSockAddress* local, bool wait)
{
  GSocketError err;

  if (m_socket)
  {
    // Shutdown and destroy the socket
    Close();
    delete m_socket;
  }

  m_socket = GSocket_new();
  m_connected = false;
  m_establishing = false;

  if (!m_socket)
    return false;

  m_socket->SetTimeout(m_timeout * 1000);
  m_socket->SetCallback(GSOCK_INPUT_FLAG | GSOCK_OUTPUT_FLAG |
                                GSOCK_LOST_FLAG | GSOCK_CONNECTION_FLAG,
                                wx_socket_callback, (char *)this);

  // If wait == false, then the call should be nonblocking.
  // When we are finished, we put the socket to blocking mode
  // again.

  if (!wait)
    m_socket->SetNonBlocking(1);

  // Reuse makes sense for clients too, if we are trying to rebind to the same port
  if (GetFlags() & wxSOCKET_REUSEADDR)
  {
    m_socket->SetReusable();
  }

  // If no local address was passed and one has been set, use the one that was Set
  if (!local && m_localAddress.GetAddress())
  {
    local = &m_localAddress;
  }

  // Bind to the local IP address and port, when provided
  if (local)
  {
    GAddress* la = local->GetAddress();

    if (la && la->m_addr)
      m_socket->SetLocal(la);
  }
  
   switch (m_proxy_type)
   {
     case wxPROXY_NONE:
       m_socket->SetPeer(addr_man.GetAddress());
       err = m_socket->Connect(GSOCK_STREAMED);
       break;
     case wxPROXY_SOCKS5:
       err = ConnectSOCKS5(addr_man);
       break;
     case wxPROXY_SOCKS4:
       err = ConnectSOCKS4(addr_man);
       break;
     case wxPROXY_SOCKS4a:
       err = ConnectSOCKS4(addr_man, true);
       break;
     case wxPROXY_HTTP:
       err = ConnectHTTP(addr_man);
       break;
     default:
       wxASSERT_MSG(0,wxT("Invalid proxy type in Connect()"));
       err = GSOCK_INVSOCK;
       break;
   }

  if (!wait)
    m_socket->SetNonBlocking(0);

  if (err != GSOCK_NOERROR)
  {
    if (err == GSOCK_WOULDBLOCK)
      m_establishing = true;
    
    m_connected = false;
  }
  else
    m_connected = true;

  return m_connected;
}

bool wxSocketClient::Connect(wxSockAddress& addr_man, bool wait)
{
    return (DoConnect(addr_man, NULL, wait));
}

bool wxSocketClient::Connect(wxSockAddress& addr_man, wxSockAddress& local, bool wait)
{
    return (DoConnect(addr_man, &local, wait));
}

bool wxSocketClient::WaitOnConnect(long seconds, long milliseconds)
{
    if (m_connected)                      // Already connected
        return true;

    if (!m_establishing || !m_socket)     // No connection in progress
        return false;

    return _Wait(seconds, milliseconds, GSOCK_CONNECTION_FLAG |
                                        GSOCK_LOST_FLAG);
}

void wxSocketClient::SetProxy(wxIPV4address& addr, wxSocketProxyType type, wxString login, wxString password) {
  wxCHECK_RET((type > wxPROXY_NONE) && (type < wxPROXY_INVALID), wxT("Invalid proxy type in SetProxy"));  
  m_proxy_addr = addr;
  m_proxy_type = type;
  m_proxy_login = login;
  m_proxy_passwd = password;
}

GSocketError wxSocketClient::ConnectSOCKS4(wxSockAddress& destination, bool socks4a) {
  
  unsigned int total_len = 0;
  
  // Proxy Connect() code. Always blocking (at least for now).
  m_socket->SetNonBlocking(0);
  
  if (m_socket->SetPeer(m_proxy_addr.GetAddress()) != GSOCK_NOERROR)
    return GSOCK_INVSOCK;
  
  if (m_socket->Connect(GSOCK_STREAMED) != GSOCK_NOERROR)
    return GSOCK_INVSOCK;
  
  wxIPV4address* destination_ptr = dynamic_cast<wxIPV4address*>(&destination);
  wxCHECK_MSG(destination_ptr, GSOCK_INVSOCK, wxT("Attempted to use proxy connection to a non-IPv4 address")); 
  
  // Ok, we connected to the proxy server. Let's request the connection.
  unsigned char request_buffer[512]; // Should be enough
  request_buffer[0] = 0x04; // SOCKS version
  request_buffer[1] = 0x01; // Command: Connect
  PokeUInt16_BE(request_buffer+2, destination_ptr->Service()); // Endianess and aligment-aware.
  long unsigned int ip = -1;
  wxASSERT(wxIPV4address::CheckStringIP(destination_ptr->IPAddress(), ip));
  if ((ip == (long unsigned int)-1) && !socks4a)
  {
    // Can't solve the destination hostname, and we have no SOCKS4a extensions
    m_socket->Shutdown();
    return GSOCK_INVSOCK;
  }
  
  if (ip == (long unsigned int)-1)
  {
    wxASSERT(socks4a);
    // 0.0.0.1 is sent on socks4a when you can't solve the hostname.
    PokeUInt32(request_buffer+4, 16777216 /* 0.0.0.1 */);
  }
  else
  {
    // Solved address on SOCKS4 / SOCKS4a
    PokeUInt32(request_buffer+4, ip); 
  }
  const wxWX2MBbuf login = wxConvLocal.cWX2MB(m_proxy_login);
  int login_len = strlen((const char*)login);
  wxCHECK_MSG(8+login_len+1 < 512, GSOCK_INVSOCK, wxT("Too long request on proxy data"));
  memcpy(request_buffer+8, (const char*)login, login_len);
  request_buffer[8+login_len] = '\0';
  total_len = 1 + 1 + 2 + 4 + login_len + 1;
  if (ip == (long unsigned int)-1)
  {
    wxASSERT(socks4a);
    // SOCKS4a that couldn't solve the IP, add the hostname to the end.
    const wxWX2MBbuf host = wxConvLocal.cWX2MB(destination_ptr->OrigHostname());
    int host_len = strlen((const char*)host);
    wxCHECK_MSG(total_len+host_len + 1 < 512, GSOCK_INVSOCK, wxT("Too long request on proxy data"));
    memcpy(request_buffer+total_len,(const char*)host, host_len);
    request_buffer[total_len+host_len] = '\0';
    total_len += host_len + 1 ;
  }

  int old_flags = m_flags;
  
  m_connected = true;
  m_flags = wxSOCKET_BLOCK | wxSOCKET_WAITALL;

  unsigned long old_timeout = m_timeout;
  SetTimeout(60);  // 60 seconds for the server to reply.

  Write(request_buffer,total_len);
  
  if (Error() || LastCount() != total_len)
  {
    m_connected = false;
    m_flags = old_flags;
    m_socket->Shutdown();
    return GSOCK_INVSOCK;
  }
  
  // Ok, let's see what the server says.
  
  unsigned char reply[8]; // Fixed size.
  
  Read((char*)reply, 8);
  
  if (Error() || LastCount() != 8)
  {
    SetTimeout(old_timeout);
    m_connected = false;
    m_flags = old_flags;
    m_socket->Shutdown();
    return GSOCK_INVSOCK;
  }
  
  if (reply[0] != 0 || reply[1] != 90)
  {
    // Proxy refused connection.
    SetTimeout(old_timeout);
    m_connected = false;
    m_flags = old_flags;
    m_socket->Shutdown();
    return GSOCK_INVSOCK;    
  }
  
  // Everything is ok. Restore old status.
  
  m_flags = old_flags;  
  SetTimeout(old_timeout);
  
  return GSOCK_NOERROR;
}


GSocketError wxSocketClient::ConnectSOCKS5(wxSockAddress& destination) {
  // Proxy Connect() code. Always blocking (at least for now).
  m_socket->SetNonBlocking(0);  
  return GSOCK_NOERROR;
}

GSocketError wxSocketClient::ConnectHTTP(wxSockAddress& destination) {
  // Proxy Connect() code. Always blocking (at least for now).
  m_socket->SetNonBlocking(0);  
  
  return GSOCK_NOERROR;
}

// ==========================================================================
// wxDatagramSocket
// ==========================================================================

/* NOTE: experimental stuff - might change */

wxDatagramSocket::wxDatagramSocket( const wxSockAddress& addr,
                                    wxSocketFlags flags )
                : wxSocketBase( flags, wxSOCKET_DATAGRAM )
{
    // Create the socket
    m_socket = GSocket_new();

    if(!m_socket)
    {
        wxFAIL_MSG( _T("datagram socket not new'd") );
        return;
    }
    // Setup the socket as non connection oriented
    m_socket->SetLocal(addr.GetAddress());
    if( m_socket->SetNonOriented() != GSOCK_NOERROR )
    {
        delete m_socket;
        m_socket = NULL;
        return;
    }

    // Initialize all stuff
    m_connected = false;
    m_establishing = false;
    m_socket->SetTimeout( m_timeout );
    m_socket->SetCallback( GSOCK_INPUT_FLAG | GSOCK_OUTPUT_FLAG |
                           GSOCK_LOST_FLAG | GSOCK_CONNECTION_FLAG,
                           wx_socket_callback, (char*)this );
}

wxDatagramSocket& wxDatagramSocket::RecvFrom( wxSockAddress& addr,
                                              void* buf,
                                              wxUint32 nBytes )
{
  wxCHECK_MSG( m_socket, (*this), _T("Socket not initialised") );
  if (m_connected)
  {
    // This is a Connected socket. We have to fail if the addr is different.
    #warning This will be cleaned when GAddress becomes a class.
    GAddress* gsock_addr = m_socket->GetPeer();
    GAddress* dest_addr = addr.GetAddress();
    bool matches = (GAddress_INET_GetHostAddress(gsock_addr) == GAddress_INET_GetHostAddress(dest_addr))
                   && (GAddress_INET_GetPort(gsock_addr) == GAddress_INET_GetPort(dest_addr));    
    wxCHECK_MSG( matches, (*this), _T("Attempt to send to a different destination address on a connected wxDatagramSocket - use Read(buf, nBytes) instead."));
  }

  Read(buf, nBytes);
  GetPeer(addr);
  return (*this);
}

wxDatagramSocket& wxDatagramSocket::SendTo( const wxSockAddress& addr,
                                            const void* buf,
                                            wxUint32 nBytes )
{
  wxCHECK_MSG( m_socket, (*this), _T("Socket not initialised") );

  if (m_connected)
  {
    // This is a Connected socket. We have to fail if the addr is different.
    #warning This will be cleaned when GAddress becomes a class.
    GAddress* gsock_addr = m_socket->GetPeer();
    GAddress* dest_addr = addr.GetAddress();
    bool matches = (GAddress_INET_GetHostAddress(gsock_addr) == GAddress_INET_GetHostAddress(dest_addr))
                   && (GAddress_INET_GetPort(gsock_addr) == GAddress_INET_GetPort(dest_addr));
    wxCHECK_MSG( matches, (*this), _T("Attempt to send to a different destination address on a connected wxDatagramSocket - use Write(buf, nBytes) instead."));
  }

  m_socket->SetPeer(addr.GetAddress());
  Write(buf, nBytes);
  return (*this);
}

bool wxDatagramSocket::Connect(wxSockAddress& addr)
{
  wxCHECK_MSG( m_socket, false, _T("Socket not initialised") );

  m_socket->SetPeer(addr.GetAddress());
  int err = m_socket->Connect(GSOCK_UNSTREAMED);

  if (err != GSOCK_NOERROR)
  {
    return false;
  }

  m_connected = true;

  return true;
}

// ==========================================================================
// wxSocketModule
// ==========================================================================

class wxSocketModule : public wxModule
{
public:
    virtual bool OnInit()
    {
        // wxSocketBase will call GSocket_Init() itself when/if needed
        return true;
    }

    virtual void OnExit()
    {
        if ( wxSocketBase::IsInitialized() )
            wxSocketBase::Shutdown();
    }

private:
    DECLARE_DYNAMIC_CLASS(wxSocketModule)
};

IMPLEMENT_DYNAMIC_CLASS(wxSocketModule, wxModule)

#endif
  // wxUSE_SOCKETS
