#include "ReliableRouter.h"
#include "Default.h"
#include "MeshModule.h"
#include "MeshTypes.h"
#include "configuration.h"
#include "mesh-pb-constants.h"
#include "modules/NodeInfoModule.h"

// ReliableRouter::ReliableRouter() {}

/**
 * If the message is want_ack, then add it to a list of packets to retransmit.
 * If we run out of retransmissions, send a nak packet towards the original client to indicate failure.
 */
ErrorCode ReliableRouter::send(meshtastic_MeshPacket *p)
{
    if (p->want_ack) {
        // If someone asks for acks on broadcast, we need the hop limit to be at least one, so that first node that receives our
        // message will rebroadcast.  But asking for hop_limit 0 in that context means the client app has no preference on hop
        // counts and we want this message to get through the whole mesh, so use the default.
        if (p->hop_limit == 0) {
            p->hop_limit = Default::getConfiguredOrDefaultHopLimit(config.lora.hop_limit);
        }

        auto copy = packetPool.allocCopy(*p);
        startRetransmission(copy);
    }

    /* If we have pending retransmissions, add the airtime of this packet to it, because during that time we cannot receive an
       (implicit) ACK. Otherwise, we might retransmit too early.
     */
    for (auto i = pending.begin(); i != pending.end(); i++) {
        if (i->first.id != p->id) {
            i->second.nextTxMsec += iface->getPacketTime(p);
        }
    }

    return p->to == NODENUM_BROADCAST ? FloodingRouter::send(p) : NextHopRouter::send(p);
}

bool ReliableRouter::shouldFilterReceived(const meshtastic_MeshPacket *p)
{
    // Note: do not use getFrom() here, because we want to ignore messages sent from phone
    if (p->from == getNodeNum()) {
        printPacket("Rx someone rebroadcasting for us", p);

        // We are seeing someone rebroadcast one of our broadcast attempts.
        // If this is the first time we saw this, cancel any retransmissions we have queued up and generate an internal ack for
        // the original sending process.

        // This "optimization", does save lots of airtime. For DMs, you also get a real ACK back
        // from the intended recipient.
        auto key = GlobalPacketId(getFrom(p), p->id);
        auto old = findPendingPacket(key);
        if (old) {
            LOG_DEBUG("generating implicit ack\n");
            // NOTE: we do NOT check p->wantAck here because p is the INCOMING rebroadcast and that packet is not expected to be
            // marked as wantAck
            sendAckNak(meshtastic_Routing_Error_NONE, getFrom(p), p->id, old->packet->channel);

            stopRetransmission(key);
        } else {
            LOG_DEBUG("didn't find pending packet\n");
        }
    }

    /* At this point we have already deleted the pending retransmission if this packet was an (implicit) ACK to it.
       Now for all other pending retransmissions, we have to add the airtime of this received packet to the retransmission timer,
       because while receiving this packet, we could not have received an (implicit) ACK for it.
       If we don't add this, we will likely retransmit too early.
    */
    for (auto i = pending.begin(); i != pending.end(); i++) {
        i->second.nextTxMsec += iface->getPacketTime(p);
    }

    /* Resend implicit ACKs for repeated packets (hopStart equals hopLimit);
     * this way if an implicit ACK is dropped and a packet is resent we'll rebroadcast again.
     * Resending real ACKs is omitted, as you might receive a packet multiple times due to flooding and
     * flooding this ACK back to the original sender already adds redundancy. */
    bool isRepeated = p->hop_start == 0 ? (p->hop_limit == HOP_RELIABLE) : (p->hop_start == p->hop_limit);
    if (wasSeenRecently(p, false) && isRepeated && !MeshModule::currentReply && p->to != nodeDB->getNodeNum()) {
        LOG_DEBUG("Resending implicit ack for a repeated floodmsg\n");
        meshtastic_MeshPacket *tosend = packetPool.allocCopy(*p);
        tosend->hop_limit--; // bump down the hop count
        Router::send(tosend);
    }

    return p->to == NODENUM_BROADCAST ? FloodingRouter::shouldFilterReceived(p) : NextHopRouter::shouldFilterReceived(p);
}

/**
 * If we receive a want_ack packet (do not check for wasSeenRecently), send back an ack (this might generate multiple ack sends in
 * case the our first ack gets lost)
 *
 * If we receive an ack packet (do check wasSeenRecently), clear out any retransmissions and
 * forward the ack to the application layer.
 *
 * If we receive a nak packet (do check wasSeenRecently), clear out any retransmissions
 * and forward the nak to the application layer.
 *
 * Otherwise, let superclass handle it.
 */
void ReliableRouter::sniffReceived(const meshtastic_MeshPacket *p, const meshtastic_Routing *c)
{
    NodeNum ourNode = getNodeNum();

    if (p->to == ourNode) { // ignore ack/nak/want_ack packets that are not address to us (we only handle 0 hop reliability)
        if (p->want_ack) {
            if (MeshModule::currentReply) {
                LOG_DEBUG("Some other module has replied to this message, no need for a 2nd ack\n");
            } else if (p->which_payload_variant == meshtastic_MeshPacket_decoded_tag) {
                sendAckNak(meshtastic_Routing_Error_NONE, getFrom(p), p->id, p->channel, p->hop_start, p->hop_limit);
            } else if (p->which_payload_variant == meshtastic_MeshPacket_encrypted_tag && p->channel == 0 &&
                       (nodeDB->getMeshNode(p->from) == nullptr || nodeDB->getMeshNode(p->from)->user.public_key.size == 0)) {
                LOG_INFO("This looks like it might be a PKI packet from an unknown node, so send PKI_UNKNOWN_PUBKEY\n");
                sendAckNak(meshtastic_Routing_Error_PKI_UNKNOWN_PUBKEY, getFrom(p), p->id, channels.getPrimaryIndex(),
                           p->hop_start, p->hop_limit);
            } else {
                // Send a 'NO_CHANNEL' error on the primary channel if want_ack packet destined for us cannot be decoded
                sendAckNak(meshtastic_Routing_Error_NO_CHANNEL, getFrom(p), p->id, channels.getPrimaryIndex(), p->hop_start,
                           p->hop_limit);
            }
        }
        if (p->which_payload_variant == meshtastic_MeshPacket_decoded_tag && c &&
            c->error_reason == meshtastic_Routing_Error_PKI_UNKNOWN_PUBKEY) {
            if (owner.public_key.size == 32) {
                LOG_INFO("This seems like a remote PKI decrypt failure, so send a NodeInfo");
                nodeInfoModule->sendOurNodeInfo(p->from, false, p->channel, true);
            }
        }
        // We consider an ack to be either a !routing packet with a request ID or a routing packet with !error
        PacketId ackId = ((c && c->error_reason == meshtastic_Routing_Error_NONE) || !c) ? p->decoded.request_id : 0;

        // A nak is a routing packt that has an  error code
        PacketId nakId = (c && c->error_reason != meshtastic_Routing_Error_NONE) ? p->decoded.request_id : 0;

        // We intentionally don't check wasSeenRecently, because it is harmless to delete non existent retransmission records
        if (ackId || nakId) {
            if (ackId) {
                LOG_DEBUG("Received an ack for 0x%x, stopping retransmissions\n", ackId);
                stopRetransmission(p->to, ackId);
            } else {
                LOG_DEBUG("Received a nak for 0x%x, stopping retransmissions\n", nakId);
                stopRetransmission(p->to, nakId);
            }
        }
    }

    p->to == NODENUM_BROADCAST ? FloodingRouter::sniffReceived(p, c) : NextHopRouter::sniffReceived(p, c);
}