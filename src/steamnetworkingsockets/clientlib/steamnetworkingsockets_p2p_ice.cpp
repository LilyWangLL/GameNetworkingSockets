//====== Copyright Valve Corporation, All rights reserved. ====================

#include "steamnetworkingsockets_p2p_ice.h"
#include "steamnetworkingsockets_udp.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE

extern "C" {
CreateICESession_t g_SteamNetworkingSockets_CreateICESessionFunc = nullptr;
}

// Put everything in a namespace, so we don't violate the one definition rule
namespace SteamNetworkingSocketsLib {

/////////////////////////////////////////////////////////////////////////////
//
// CConnectionTransportP2PSDR
//
/////////////////////////////////////////////////////////////////////////////

CConnectionTransportP2PICE::CConnectionTransportP2PICE( CSteamNetworkConnectionP2P &connection )
: CConnectionTransportUDPBase( connection )
, m_pICESession( nullptr )
, m_pszNeedToSendSignalReason( nullptr )
, m_usecSendSignalDeadline( INT64_MAX )
, m_nRemoteCandidatesRevision( 0 )
, m_nLocalCandidatesRevision( 0 )
{
	m_ping.Reset();
	m_usecTimeLastRecv = 0;
	m_usecInFlightReplyTimeout = 0;
	m_nReplyTimeoutsSinceLastRecv = 0;
	m_nTotalPingsSent = 0;
	m_bNeedToConfirmEndToEndConnectivity = true;
}

CConnectionTransportP2PICE::~CConnectionTransportP2PICE()
{
	Assert( !m_pICESession );
}

void CConnectionTransportP2PICE::TransportPopulateConnectionInfo( SteamNetConnectionInfo_t &info ) const
{
	CConnectionTransport::TransportPopulateConnectionInfo( info );

	// FIXME Need to rev the ice session interface version so that I get back this info!
	info.m_eTransportKind = k_ESteamNetTransport_UDP;
}

void CConnectionTransportP2PICE::GetDetailedConnectionStatus( SteamNetworkingDetailedConnectionStatus &stats, SteamNetworkingMicroseconds usecNow )
{
	// FIXME Need to indicate whether we are relayed or were able to pierce NAT
	CConnectionTransport::GetDetailedConnectionStatus( stats, usecNow );
}

void CConnectionTransportP2PICE::TransportFreeResources()
{
	if ( m_pICESession )
	{
		m_pICESession->Destroy();
		m_pICESession = nullptr;
	}
	ClearNextThinkTime();

	CConnectionTransport::TransportFreeResources();
}

void CConnectionTransportP2PICE::Init()
{
	if ( !g_SteamNetworkingSockets_CreateICESessionFunc )
	{
		NotifyConnectionFailed( k_ESteamNetConnectionEnd_Misc_InternalError, "CreateICESession factory not set" );
		return;
	}

	SpewType( LogLevel_P2PRendezvous(), "[%s] Using STUN server list: %s\n", ConnectionDescription(), m_connection.m_connectionConfig.m_P2P_STUN_ServerList.Get().c_str() );
	{
		CUtlVectorAutoPurge<char *> tempStunServers;
		V_AllocAndSplitString( m_connection.m_connectionConfig.m_P2P_STUN_ServerList.Get().c_str(), ",", tempStunServers );
		for ( const char *pszAddress: tempStunServers )
		{
			std::string server;

			// Add prefix, unless they already supplied it
			if ( V_strnicmp( pszAddress, "stun:", 5 ) != 0 )
				server = "stun:";
			server.append( pszAddress );

			m_vecStunServers.push_back( std::move( server ) );
		}
	}

	m_pICESession = (*g_SteamNetworkingSockets_CreateICESessionFunc)( this, ICESESSION_INTERFACE_VERSION );
	if ( !m_pICESession )
	{
		NotifyConnectionFailed( k_ESteamNetConnectionEnd_Misc_InternalError, "CreateICESession failed" );
		return;
	}

	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ETW
		m_pICESession->SetWriteEvent_setsockopt( ETW_webrtc_setsockopt );
		m_pICESession->SetWriteEvent_send( ETW_webrtc_send );
		m_pICESession->SetWriteEvent_sendto( ETW_webrtc_sendto );
	#endif
}

void CConnectionTransportP2PICE::PopulateRendezvousMsg( CMsgSteamNetworkingP2PRendezvous &msg, SteamNetworkingMicroseconds usecNow )
{
	m_pszNeedToSendSignalReason = nullptr;
	m_usecSendSignalDeadline = INT64_MAX;

	CMsgWebRTCRendezvous *pMsgWebRTC = msg.mutable_webrtc();

	// Any un-acked candidates that we are ready to (re)try
	for ( LocalCandidate &s: m_vecLocalUnAckedCandidates )
	{

		// Not yet ready to retry sending?
		if ( !pMsgWebRTC->has_first_candidate_revision() )
		{
			if ( s.m_usecRTO > usecNow )
				continue; // We've sent.  Don't give up yet.

			// Start sending from this guy forward
			pMsgWebRTC->set_first_candidate_revision( s.m_nRevision );
		}

		*pMsgWebRTC->add_candidates() = s.candidate;

		s.m_usecRTO = usecNow + k_nMillion/2; // Reset RTO

		// If we have a ton of candidates, don't send
		// too many in a single message
		if ( pMsgWebRTC->candidates_size() > 10 )
			break;
	}

	// Go ahead and always ack, even if we don't need to, because this is small
	if ( m_nRemoteCandidatesRevision > 0 )
		pMsgWebRTC->set_ack_candidates_revision( m_nRemoteCandidatesRevision );

}

void CConnectionTransportP2PICE::RecvRendezvous( const CMsgWebRTCRendezvous &msg, SteamNetworkingMicroseconds usecNow )
{
	// Safety
	if ( !m_pICESession )
	{
		NotifyConnectionFailed( k_ESteamNetConnectionEnd_Misc_InternalError, "No IICESession?" );
		return;
	}

	// Check if they are acking that they have received candidates
	if ( msg.has_ack_candidates_revision() )
	{

		// Remove any candidates from our list that are being acked
		while ( !m_vecLocalUnAckedCandidates.empty() && m_vecLocalUnAckedCandidates[0].m_nRevision <= msg.ack_candidates_revision() )
			erase_at( m_vecLocalUnAckedCandidates, 0 );

		// Check anything ready to retry now
		for ( const LocalCandidate &s: m_vecLocalUnAckedCandidates )
		{
			if ( s.m_usecRTO < usecNow )
			{
				ScheduleSendSignal( "SendCandidates" );
				break;
			}
		}
	}

	// Check if they sent candidate update.
	if ( msg.has_first_candidate_revision() )
	{

		// Send an ack, no matter what
		ScheduleSendSignal( "AckCandidatesRevision" );

		// Only process them if it was the next chunk we were expecting.
		if ( msg.first_candidate_revision() == m_nRemoteCandidatesRevision+1 )
		{

			// Take the update
			for ( const CMsgWebRTCRendezvous_Candidate &c: msg.candidates() )
			{
				if ( m_pICESession->BAddRemoteIceCandidate( c.sdpm_id().c_str(), c.sdpm_line_index(), c.candidate().c_str() ) )
				{
					SpewType( LogLevel_P2PRendezvous(), "[%s] Processed remote Ice Candidate %s\n", ConnectionDescription(), c.ShortDebugString().c_str() );
				}
				else
				{
					SpewWarning( "[%s] Ignoring candidate %s\n", ConnectionDescription(), c.ShortDebugString().c_str() );
				}
				++m_nRemoteCandidatesRevision;
			}
		}
	}
}

void CConnectionTransportP2PICE::NotifyConnectionFailed( int nReasonCode, const char *pszReason )
{
	SteamDatagramTransportLock::AssertHeldByCurrentThread();

	// Remember reason code, if we didn't already set one
	if ( Connection().m_nICECloseCode == 0 )
	{
		SpewType( LogLevel_P2PRendezvous(), "[%s] ICE failed %d %s\n", ConnectionDescription(), nReasonCode, pszReason );
		Connection().m_nICECloseCode = nReasonCode;
		V_strcpy_safe( Connection().m_szICECloseMsg, pszReason );
	}

	QueueSelfDestruct();
}

void CConnectionTransportP2PICE::QueueSelfDestruct()
{
	// NOTE: Do *not* attempt to delete the ICE session here.  We don't have enough
	//       context if that is safe to do.

	// Queue us for deletion
	if ( Connection().m_pTransportICEPendingDelete )
	{
		// Already queued for delete
		Assert( Connection().m_pTransportICEPendingDelete == this );
	}
	else
	{
		Connection().m_pTransportICEPendingDelete = this;
		Assert( Connection().m_pTransportICE == this );
		Connection().m_pTransportICE = nullptr;
	}

	// Make sure we clean ourselves up as soon as it is safe to do so
	SetNextThinkTimeASAP();
}

void CConnectionTransportP2PICE::ScheduleSendSignal( const char *pszReason )
{
	SteamNetworkingMicroseconds usecDeadline = SteamNetworkingSockets_GetLocalTimestamp() + 10*1000;
	if ( !m_pszNeedToSendSignalReason || m_usecSendSignalDeadline > usecDeadline )
	{
		m_pszNeedToSendSignalReason = pszReason;
		m_usecSendSignalDeadline = usecDeadline;
	}
	EnsureMinThinkTime( m_usecSendSignalDeadline );
}

void CConnectionTransportP2PICE::Think( SteamNetworkingMicroseconds usecNow )
{
	// Are we dead?
	if ( !m_pICESession || Connection().m_pTransportICEPendingDelete )
	{
		Connection().CheckCleanupICE();
		// We could be deleted here!
		return;
	}

	// We only need to take action while connecting, or trying to connect
	if ( ConnectionState() != k_ESteamNetworkingConnectionState_FindingRoute && ConnectionState() != k_ESteamNetworkingConnectionState_Connected )
	{
		// Will we get a state transition wakeup call?
		return;
	}

	SteamNetworkingMicroseconds usecNextThink = k_nThinkTime_Never;

	// Check for reply timeout
	if ( m_usecInFlightReplyTimeout )
	{
		if ( m_usecInFlightReplyTimeout < usecNow )
		{
			m_usecInFlightReplyTimeout = 0;
			++m_nReplyTimeoutsSinceLastRecv;
			if ( m_nReplyTimeoutsSinceLastRecv > 2 && !m_bNeedToConfirmEndToEndConnectivity )
			{
				m_bNeedToConfirmEndToEndConnectivity = true;
				SpewWarning( "[%s] ICE end-to-end connectivity needs to be re-confirmed, %d consecutive timeouts\n", ConnectionDescription(), m_nReplyTimeoutsSinceLastRecv );
				Connection().TransportEndToEndConnectivityChanged( this );
			}
		}
		else
		{
			usecNextThink = std::min( usecNextThink, m_usecInFlightReplyTimeout );
		}
	}

	// Check for sending ping requests
	if ( m_usecInFlightReplyTimeout == 0 )
	{
		// Check for pinging as fast as possible until we get an initial ping sample.
		if (
			m_nTotalPingsSent < 10 // Minimum number of tries, period
			|| (
				(
					m_nReplyTimeoutsSinceLastRecv < 3 // we don't look like we're failing
					|| Connection().m_pTransport == this // they have selected us
					|| Connection().m_pTransport == nullptr // They haven't selected anybody
				)
				&& (
					// Some reason to establish connectivity or collect more data
					m_bNeedToConfirmEndToEndConnectivity
					|| m_ping.m_nSmoothedPing < 0
					|| m_ping.m_nValidPings < V_ARRAYSIZE(m_ping.m_arPing)
					|| m_ping.m_nTotalPingsReceived < 10
				)
			)
		) {
			CMsgSteamSockets_UDP_ICEPingCheck msgPing;
			msgPing.set_send_timestamp( usecNow );
			msgPing.set_from_connection_id( ConnectionIDLocal() );
			msgPing.set_to_connection_id( ConnectionIDRemote() );
			SendMsg( k_ESteamNetworkingUDPMsg_ICEPingCheck, msgPing );
			TrackSentPingRequest( usecNow, false );

			Assert( m_usecInFlightReplyTimeout > usecNow );
			usecNextThink = std::min( usecNextThink, m_usecInFlightReplyTimeout );
		}
	}

	// Check for sending a signal
	{

		bool bSendSignal = true;
		if ( usecNow < m_usecSendSignalDeadline )
		{
			if ( m_vecLocalUnAckedCandidates.empty() || m_vecLocalUnAckedCandidates[0].m_usecRTO > usecNow )
				bSendSignal = false;
			else
				m_pszNeedToSendSignalReason = "CandidateRTO";
		}

		if ( bSendSignal )
		{
			Assert( m_pszNeedToSendSignalReason );

			// Send a signal
			CMsgSteamNetworkingP2PRendezvous msgRendezvous;
			Connection().SetRendezvousCommonFieldsAndSendSignal( msgRendezvous, usecNow, m_pszNeedToSendSignalReason );
		}

		Assert( m_usecSendSignalDeadline > usecNow );

		usecNextThink = std::min( usecNextThink, m_usecSendSignalDeadline );
		if ( !m_vecLocalUnAckedCandidates.empty() && m_vecLocalUnAckedCandidates[0].m_usecRTO > 0 )
			usecNextThink = std::min( usecNextThink, m_vecLocalUnAckedCandidates[0].m_usecRTO );
	}

	EnsureMinThinkTime( usecNextThink );
}

void CConnectionTransportP2PICE::TrackSentPingRequest( SteamNetworkingMicroseconds usecNow, bool bAllowDelayedReply )
{
	if ( m_usecInFlightReplyTimeout == 0 )
	{
		m_usecInFlightReplyTimeout = usecNow + m_ping.CalcConservativeTimeout();
		if ( bAllowDelayedReply )
			m_usecInFlightReplyTimeout += k_usecSteamDatagramRouterPendClientPing;
		EnsureMinThinkTime( m_usecInFlightReplyTimeout );
	}
	m_ping.m_usecTimeLastSentPingRequest = usecNow;
}

#define ParseProtobufBody( pvMsg, cbMsg, CMsgCls, msgVar ) \
	CMsgCls msgVar; \
	if ( !msgVar.ParseFromArray( pvMsg, cbMsg ) ) \
	{ \
		ReportBadUDPPacketFromConnectionPeer( # CMsgCls, "Protobuf parse failed." ); \
		return; \
	}

#define ParsePaddedPacket( pvPkt, cbPkt, CMsgCls, msgVar ) \
	CMsgCls msgVar; \
	{ \
		if ( cbPkt < k_cbSteamNetworkingMinPaddedPacketSize ) \
		{ \
			ReportBadUDPPacketFromConnectionPeer( # CMsgCls, "Packet is %d bytes, must be padded to at least %d bytes.", cbPkt, k_cbSteamNetworkingMinPaddedPacketSize ); \
			return; \
		} \
		const UDPPaddedMessageHdr *hdr =  (const UDPPaddedMessageHdr *)( pvPkt ); \
		int nMsgLength = LittleWord( hdr->m_nMsgLength ); \
		if ( nMsgLength <= 0 || int(nMsgLength+sizeof(UDPPaddedMessageHdr)) > cbPkt ) \
		{ \
			ReportBadUDPPacketFromConnectionPeer( # CMsgCls, "Invalid encoded message length %d.  Packet is %d bytes.", nMsgLength, cbPkt ); \
			return; \
		} \
		if ( !msgVar.ParseFromArray( hdr+1, nMsgLength ) ) \
		{ \
			ReportBadUDPPacketFromConnectionPeer( # CMsgCls, "Protobuf parse failed." ); \
			return; \
		} \
	}

void CConnectionTransportP2PICE::ProcessPacket( const uint8_t *pPkt, int cbPkt, SteamNetworkingMicroseconds usecNow )
{
	Assert( cbPkt >= 1 ); // Caller should have checked this
	ETW_ICEProcessPacket( m_connection.m_hConnectionSelf, cbPkt );

	m_usecTimeLastRecv = usecNow;
	m_usecInFlightReplyTimeout = 0;
	m_nReplyTimeoutsSinceLastRecv = 0;

	// Data packet is the most common, check for it first.  Also, does stat tracking.
	if ( *pPkt & 0x80 )
	{
		Received_Data( pPkt, cbPkt, usecNow );
		return;
	}

	// Track stats for other packet types.
	m_connection.m_statsEndToEnd.TrackRecvPacket( cbPkt, usecNow );

	if ( *pPkt == k_ESteamNetworkingUDPMsg_ConnectionClosed )
	{
		ParsePaddedPacket( pPkt, cbPkt, CMsgSteamSockets_UDP_ConnectionClosed, msg )
		Received_ConnectionClosed( msg, usecNow );
	}
	else if ( *pPkt == k_ESteamNetworkingUDPMsg_NoConnection )
	{
		ParseProtobufBody( pPkt+1, cbPkt-1, CMsgSteamSockets_UDP_NoConnection, msg )
		Received_NoConnection( msg, usecNow );
	}
	else if ( *pPkt == k_ESteamNetworkingUDPMsg_ICEPingCheck )
	{
		ParseProtobufBody( pPkt+1, cbPkt-1, CMsgSteamSockets_UDP_ICEPingCheck, msg )
		Received_PingCheck( msg, usecNow );
	}
	else
	{
		ReportBadUDPPacketFromConnectionPeer( "packet", "Lead byte 0x%02x not a known message ID", *pPkt );
	}
}

void CConnectionTransportP2PICE::Received_PingCheck( const CMsgSteamSockets_UDP_ICEPingCheck &msg, SteamNetworkingMicroseconds usecNow )
{
	// FIXME check to/from connection ID

	if ( msg.has_recv_timestamp() )
	{
		SteamNetworkingMicroseconds usecElapsed = usecNow - msg.recv_timestamp();
		if ( usecElapsed < 0 || usecElapsed > 2*k_nMillion )
		{
			ReportBadUDPPacketFromConnectionPeer( "WeirdPingTimestamp", "Ignoring ping timestamp of %lld (%lld -> %lld)",
				(long long)usecElapsed, (long long)msg.recv_timestamp(), (long long)usecNow );
		}
		else
		{
			m_ping.ReceivedPing( ( usecElapsed + 500 ) / 1000, usecNow );

			// Check if this is the first time connectivity has changed
			if ( m_bNeedToConfirmEndToEndConnectivity )
			{
				m_bNeedToConfirmEndToEndConnectivity = false;
				SpewMsg( "[%s] ICE end-to-end connectivity confirmed, ping = %.1fms\n", ConnectionDescription(), usecElapsed*1e-3 );
				Connection().TransportEndToEndConnectivityChanged( this );
			}
		}
	}

	// Are they asking for a reply?
	if ( msg.has_send_timestamp() )
	{
		CMsgSteamSockets_UDP_ICEPingCheck pong;
		pong.set_from_connection_id( ConnectionIDLocal() );
		pong.set_to_connection_id( ConnectionIDRemote() );
		pong.set_recv_timestamp( msg.send_timestamp() );

		// We're sending a ping message.  Ask for them to ping us back again?
		// FIXME - should we match the logic in Think()?
		if ( m_ping.m_nValidPings < 3 )
		{
			pong.set_send_timestamp( usecNow );
			TrackSentPingRequest( usecNow, false );
		}

		SendMsg( k_ESteamNetworkingUDPMsg_ICEPingCheck, pong );
	}
}

void CConnectionTransportP2PICE::TransportConnectionStateChanged( ESteamNetworkingConnectionState eOldState )
{
	CConnectionTransport::TransportConnectionStateChanged( eOldState );

	switch ( ConnectionState() )
	{
		default:
			Assert( false );
		case k_ESteamNetworkingConnectionState_None:
		case k_ESteamNetworkingConnectionState_Connecting:
		case k_ESteamNetworkingConnectionState_Connected:
		case k_ESteamNetworkingConnectionState_FindingRoute:
		case k_ESteamNetworkingConnectionState_Linger:
		case k_ESteamNetworkingConnectionState_Dead:
			break;


		case k_ESteamNetworkingConnectionState_FinWait:
		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
			SendConnectionClosedOrNoConnection();
			break;

		case k_ESteamNetworkingConnectionState_ClosedByPeer:
			QueueSelfDestruct();
			break;
	}
}

bool CConnectionTransportP2PICE::SendPacket( const void *pkt, int cbPkt )
{
	if ( !m_pICESession )
		return false;

	ETW_ICESendPacket( m_connection.m_hConnectionSelf, cbPkt );
	if ( !m_pICESession->BSendData( pkt, cbPkt ) )
		return false;

	// Update stats
	m_connection.m_statsEndToEnd.TrackSentPacket( cbPkt );
	return true;
}

bool CConnectionTransportP2PICE::SendPacketGather( int nChunks, const iovec *pChunks, int cbSendTotal )
{
	if ( nChunks == 1 )
	{
		Assert( (int)pChunks->iov_len == cbSendTotal );
		SendPacket( pChunks->iov_base, pChunks->iov_len );
		return false;
	}
	if ( cbSendTotal > k_cbSteamNetworkingSocketsMaxUDPMsgLen )
	{
		Assert( false );
		return false;
	}
	uint8 pkt[ k_cbSteamNetworkingSocketsMaxUDPMsgLen ];
	uint8 *p = pkt;
	while ( nChunks > 0 )
	{
		if ( p + pChunks->iov_len > pkt+cbSendTotal )
		{
			Assert( false );
			return false;
		}
		memcpy( p, pChunks->iov_base, pChunks->iov_len );
		p += pChunks->iov_len;
		--nChunks;
		++pChunks;
	}
	Assert( p == pkt+cbSendTotal );
	return SendPacket( pkt, p-pkt );
}

bool CConnectionTransportP2PICE::BCanSendEndToEndData() const
{
	if ( !m_pICESession )
		return false;
	if ( !m_pICESession->GetWritableState() )
		return false;
	return true;
}

/////////////////////////////////////////////////////////////////////////////
//
// IICESessionDelegate handlers
//
// NOTE: These can be invoked from any thread,
// and we won't hold the lock
//
/////////////////////////////////////////////////////////////////////////////

class IConnectionTransportP2PICERunWithLock : public ISteamNetworkingSocketsRunWithLock
{
public:
	uint32 m_nConnectionIDLocal;

	virtual void RunTransportP2PICE( CConnectionTransportP2PICE *pTransport ) = 0;
private:
	virtual void Run()
	{
		CSteamNetworkConnectionBase *pConnBase = FindConnectionByLocalID( m_nConnectionIDLocal );
		if ( !pConnBase )
			return;

		// FIXME RTTI!
		CSteamNetworkConnectionP2P *pConn = dynamic_cast<CSteamNetworkConnectionP2P *>( pConnBase );
		if ( !pConn )
			return;

		if ( !pConn->m_pTransportICE )
			return;

		RunTransportP2PICE( pConn->m_pTransportICE );
	}
};


void CConnectionTransportP2PICE::Log( IICESessionDelegate::ELogPriority ePriority, const char *pszMessageFormat, ... )
{
	ESteamNetworkingSocketsDebugOutputType eType;
	switch ( ePriority )
	{
		default:	
			AssertMsg1( false, "Unknown priority %d", ePriority );
			// FALLTHROUGH

		case IICESessionDelegate::k_ELogPriorityDebug: eType = k_ESteamNetworkingSocketsDebugOutputType_Debug; break;
		case IICESessionDelegate::k_ELogPriorityVerbose: eType = k_ESteamNetworkingSocketsDebugOutputType_Verbose; break;
		case IICESessionDelegate::k_ELogPriorityInfo: eType = k_ESteamNetworkingSocketsDebugOutputType_Msg; break;
		case IICESessionDelegate::k_ELogPriorityWarning: eType = k_ESteamNetworkingSocketsDebugOutputType_Warning; break;
		case IICESessionDelegate::k_ELogPriorityError: eType = k_ESteamNetworkingSocketsDebugOutputType_Error; break;
	}

	if ( eType > g_eSteamDatagramDebugOutputDetailLevel )
		return;

	// FIXME Warning!  This can be called from any thread

	char buf[ 1024 ];
	va_list ap;
	va_start( ap, pszMessageFormat );
	V_vsprintf_safe( buf, pszMessageFormat, ap );
	va_end( ap );

	//ReallySpewType( eType, "[%s] WebRTC: %s", ConnectionDescription(), buf );
	ReallySpewType( eType, "WebRTC: %s", buf ); // FIXME would like to get the connection description
}

EICERole CConnectionTransportP2PICE::GetRole()
{
	return m_connection.m_bConnectionInitiatedRemotely ? k_EICERole_Controlled : k_EICERole_Controlling;
}

int CConnectionTransportP2PICE::GetNumStunServers()
{
	return len( m_vecStunServers );
}

const char *CConnectionTransportP2PICE::GetStunServer( int iIndex )
{
	if ( iIndex < 0 || iIndex >= len( m_vecStunServers ) )
		return nullptr;
	return m_vecStunServers[ iIndex ].c_str();
}

void CConnectionTransportP2PICE::OnIceCandidateAdded( const char *pszSDPMid, int nSDPMLineIndex, const char *pszCandidate )
{
//	// !KLUDGE! Disable local candidates, force use of STUN
//	if ( V_stristr( pszCandidate, " host " ) )
//		return;

	struct RunIceCandidateAdded : IConnectionTransportP2PICERunWithLock
	{
		CMsgWebRTCRendezvous_Candidate candidate;
		virtual void RunTransportP2PICE( CConnectionTransportP2PICE *pTransport )
		{
			SpewType( pTransport->LogLevel_P2PRendezvous(), "[%s] WebRTC OnIceCandidateAdded %s\n", pTransport->ConnectionDescription(), candidate.ShortDebugString().c_str() );

			pTransport->ScheduleSendSignal( "WebRTCCandidateAdded" );

			// Add to list of candidates that peer doesn't know about, and bump revision
			CConnectionTransportP2PICE::LocalCandidate *c = push_back_get_ptr( pTransport->m_vecLocalUnAckedCandidates );
			c->m_nRevision = ++pTransport->m_nLocalCandidatesRevision;
			c->candidate = std::move( candidate );
			c->m_usecRTO = 0;
		}
	};

	RunIceCandidateAdded *pRun = new RunIceCandidateAdded;
	pRun->m_nConnectionIDLocal = m_connection.m_unConnectionIDLocal;
	pRun->candidate.set_sdpm_id( pszSDPMid );
	pRun->candidate.set_sdpm_line_index( nSDPMLineIndex );
	pRun->candidate.set_candidate( pszCandidate );
	pRun->RunOrQueue( "WebRTC OnIceCandidateAdded" );
}

void CConnectionTransportP2PICE::DrainPacketQueue( SteamNetworkingMicroseconds usecNow )
{
	// Quickly swap into temp
	CUtlBuffer buf;
	m_mutexPacketQueue.lock();
	buf.Swap( m_bufPacketQueue );
	m_mutexPacketQueue.unlock();

	//SpewMsg( "CConnectionTransportP2PICE::DrainPacketQueue: %d bytes queued\n", buf.TellPut() );

	// Process all the queued packets
	uint8 *p = (uint8*)buf.Base();
	uint8 *end = p + buf.TellPut();
	while ( p < end )
	{
		if ( p+sizeof(int) > end )
		{
			Assert(false);
			break;
		}
		int cbPkt = *(int*)p;
		p += sizeof(int);
		if ( p + cbPkt > end )
		{
			// BUG!
			Assert(false);
			break;
		}
		ProcessPacket( p, cbPkt, usecNow );
		p += cbPkt;
	}
}

void CConnectionTransportP2PICE::OnWritableStateChanged()
{
	// FIXME - should signal to connection to trigger thinking or
	// re-evaluate transport
}

void CConnectionTransportP2PICE::OnData( const void *pPkt, size_t nSize )
{
	SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();
	const int cbPkt = int(nSize);

	if ( nSize < 1 )
	{
		ReportBadUDPPacketFromConnectionPeer( "packet", "Bad packet size: %d", cbPkt );
		return;
	}

	// See if we can process this packet (and anything queued before us)
	// immediately
	if ( SteamDatagramTransportLock::TryLock( "WebRTC Data", 0 ) )
	{
		// We can process the data now!
		//SpewMsg( "CConnectionTransportP2PICE::OnData %d bytes, process immediate\n", (int)nSize );

		// Check if queue is empty.  Note that no race conditions here.  We hold the lock,
		// which means we aren't messing with it in some other thread.  And we are in WebRTC's
		// callback, and we assume WebRTC will not call us from two threads at the same time.
		if ( m_bufPacketQueue.TellPut() > 0 )
		{
			DrainPacketQueue( usecNow );
			Assert( m_bufPacketQueue.TellPut() == 0 );
		}

		// And now process this packet
		ProcessPacket( (const uint8_t*)pPkt, cbPkt, usecNow );
		SteamDatagramTransportLock::Unlock();
		return;
	}

	// We're busy in the other thread.  We'll have to queue the data.
	// Grab the buffer lock
	m_mutexPacketQueue.lock();
	int nSaveTellPut = m_bufPacketQueue.TellPut();
	m_bufPacketQueue.PutInt( cbPkt );
	m_bufPacketQueue.Put( pPkt, cbPkt );
	m_mutexPacketQueue.unlock();

	// If the queue was empty,then we need to add a task to flush it
	// when we acquire the queue.  If it wasn't empty then a task is
	// already in the queue.  Or perhaps it was progress right now
	// in some other thread.  But if that were the case, we know that
	// it had not yet actually swapped the buffer out.  Because we had
	// the buffer lock when we checked if the queue was empty.
	if ( nSaveTellPut == 0 )
	{
		//SpewMsg( "CConnectionTransportP2PICE::OnData %d bytes, queued, added drain queue task\n", (int)nSize );
		struct RunDrainQueue : IConnectionTransportP2PICERunWithLock
		{
			virtual void RunTransportP2PICE( CConnectionTransportP2PICE *pTransport )
			{
				pTransport->DrainPacketQueue( SteamNetworkingSockets_GetLocalTimestamp() );
			}
		};

		RunDrainQueue *pRun = new RunDrainQueue;
		pRun->m_nConnectionIDLocal = m_connection.m_unConnectionIDLocal;

		// Queue it.  Don't use RunOrQueue.  We know we need to queue it,
		// since we already tried to grab the lock and failed.
		pRun->Queue( "WebRTC DrainQueue" );
	}
	else
	{
		if ( nSaveTellPut > 30000 )
		{
			SpewMsg( "CConnectionTransportP2PICE::OnData %d bytes, queued, %d previously queued LOCK PROBLEM!\n", (int)nSize, nSaveTellPut );
		}
		else
		{
			//SpewMsg( "CConnectionTransportP2PICE::OnData %d bytes, queued, %d previously queued\n", (int)nSize, nSaveTellPut );
		}
	}
}

} // namespace SteamNetworkingSocketsLib

#endif // #ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE
