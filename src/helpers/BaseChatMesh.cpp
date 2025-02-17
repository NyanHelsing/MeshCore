#include <helpers/BaseChatMesh.h>
#include <Utils.h>

mesh::Packet* BaseChatMesh::createSelfAdvert(const char* name) {
  uint8_t app_data[MAX_ADVERT_DATA_SIZE];
  uint8_t app_data_len;
  {
    AdvertDataBuilder builder(ADV_TYPE_CHAT, name);
    app_data_len = builder.encodeTo(app_data);
  }

  return createAdvert(self_id, app_data, app_data_len);
}

void BaseChatMesh::onAdvertRecv(mesh::Packet* packet, const mesh::Identity& id, uint32_t timestamp, const uint8_t* app_data, size_t app_data_len) {
  AdvertDataParser parser(app_data, app_data_len);
  if (!(parser.isValid() && parser.hasName())) {
    MESH_DEBUG_PRINTLN("onAdvertRecv: invalid app_data, or name is missing: len=%d", app_data_len);
    return;
  }

  ContactInfo* from = NULL;
  for (int i = 0; i < num_contacts; i++) {
    if (id.matches(contacts[i].id)) {  // is from one of our contacts
      from = &contacts[i];
      if (timestamp <= from->last_advert_timestamp) {  // check for replay attacks!!
        MESH_DEBUG_PRINTLN("onAdvertRecv: Possible replay attack, name: %s", from->name);
        return;
      }
      break;
    }
  }

  bool is_new = false;
  if (from == NULL) {
    is_new = true;
    if (num_contacts < MAX_CONTACTS) {
      from = &contacts[num_contacts++];
      from->id = id;
      from->out_path_len = -1;  // initially out_path is unknown
      // only need to calculate the shared_secret once, for better performance
      self_id.calcSharedSecret(from->shared_secret, id);
    } else {
      MESH_DEBUG_PRINTLN("onAdvertRecv: contacts table is full!");
      return;
    }
  }

  // update
  strncpy(from->name, parser.getName(), sizeof(from->name)-1);
  from->name[sizeof(from->name)-1] = 0;
  from->type = parser.getType();
  from->last_advert_timestamp = timestamp;

  onDiscoveredContact(*from, is_new);       // let UI know
}

int BaseChatMesh::searchPeersByHash(const uint8_t* hash) {
  int n = 0;
  for (int i = 0; i < num_contacts && n < MAX_SEARCH_RESULTS; i++) {
    if (contacts[i].id.isHashMatch(hash)) {
      matching_peer_indexes[n++] = i;  // store the INDEXES of matching contacts (for subsequent 'peer' methods)
    }
  }
  return n;
}

void BaseChatMesh::getPeerSharedSecret(uint8_t* dest_secret, int peer_idx) {
  int i = matching_peer_indexes[peer_idx];
  if (i >= 0 && i < num_contacts) {
    // lookup pre-calculated shared_secret
    memcpy(dest_secret, contacts[i].shared_secret, PUB_KEY_SIZE);
  } else {
    MESH_DEBUG_PRINTLN("getPeerSharedSecret: Invalid peer idx: %d", i);
  }
}

void BaseChatMesh::onPeerDataRecv(mesh::Packet* packet, uint8_t type, int sender_idx, const uint8_t* secret, uint8_t* data, size_t len) {
  if (type == PAYLOAD_TYPE_TXT_MSG && len > 5) {
    int i = matching_peer_indexes[sender_idx];
    if (i < 0 || i >= num_contacts) {
      MESH_DEBUG_PRINTLN("onPeerDataRecv: Invalid sender idx: %d", i);
      return;
    }

    ContactInfo& from = contacts[i];

    uint32_t timestamp;
    memcpy(&timestamp, data, 4);  // timestamp (by sender's RTC clock - which could be wrong)
    uint flags = data[4];   // message attempt number, and other flags

    // len can be > original length, but 'text' will be padded with zeroes
    data[len] = 0; // need to make a C string again, with null terminator

    //if ( ! alreadyReceived timestamp ) {
    if ((flags >> 2) == 0) {   // plain text msg?
      onMessageRecv(from, packet->isRouteFlood(), timestamp, (const char *) &data[5]);  // let UI know

      uint32_t ack_hash;    // calc truncated hash of the message timestamp + text + sender pub_key, to prove to sender that we got it
      mesh::Utils::sha256((uint8_t *) &ack_hash, 4, data, 5 + strlen((char *)&data[5]), from.id.pub_key, PUB_KEY_SIZE);

      if (packet->isRouteFlood()) {
        // let this sender know path TO here, so they can use sendDirect(), and ALSO encode the ACK
        mesh::Packet* path = createPathReturn(from.id, secret, packet->path, packet->path_len,
                                                PAYLOAD_TYPE_ACK, (uint8_t *) &ack_hash, 4);
        if (path) sendFlood(path);
      } else {
        mesh::Packet* ack = createAck(ack_hash);
        if (ack) {
          if (from.out_path_len < 0) {
            sendFlood(ack);
          } else {
            sendDirect(ack, from.out_path, from.out_path_len);
          }
        }
      }
    } else {
      MESH_DEBUG_PRINTLN("onPeerDataRecv: unsupported message type: %u", (uint32_t) (flags >> 2));
    }
  }
}

bool BaseChatMesh::onPeerPathRecv(mesh::Packet* packet, int sender_idx, const uint8_t* secret, uint8_t* path, uint8_t path_len, uint8_t extra_type, uint8_t* extra, uint8_t extra_len) {
  int i = matching_peer_indexes[sender_idx];
  if (i < 0 || i >= num_contacts) {
    MESH_DEBUG_PRINTLN("onPeerPathRecv: Invalid sender idx: %d", i);
    return false;
  }

  ContactInfo& from = contacts[i];

  // NOTE: for this impl, we just replace the current 'out_path' regardless, whenever sender sends us a new out_path.
  // FUTURE: could store multiple out_paths per contact, and try to find which is the 'best'(?)
  memcpy(from.out_path, path, from.out_path_len = path_len);  // store a copy of path, for sendDirect()

  onContactPathUpdated(from);

  if (extra_type == PAYLOAD_TYPE_ACK && extra_len >= 4) {
    // also got an encoded ACK!
    if (processAck(extra)) {
      txt_send_timeout = 0;   // matched one we're waiting for, cancel timeout timer
    }
  }
  return true;  // send reciprocal path if necessary
}

void BaseChatMesh::onAckRecv(mesh::Packet* packet, uint32_t ack_crc) {
  if (processAck((uint8_t *)&ack_crc)) {
    txt_send_timeout = 0;   // matched one we're waiting for, cancel timeout timer
    packet->markDoNotRetransmit();   // ACK was for this node, so don't retransmit
  }
}

#ifdef MAX_GROUP_CHANNELS
int BaseChatMesh::searchChannelsByHash(const uint8_t* hash, mesh::GroupChannel dest[], int max_matches) {
  int n = 0;
  for (int i = 0; i < num_channels && n < max_matches; i++) {
    if (channels[i].hash[0] == hash[0]) {
      dest[n++] = channels[i];
    }
  }
  return n;
}
#endif

void BaseChatMesh::onGroupDataRecv(mesh::Packet* packet, uint8_t type, const mesh::GroupChannel& channel, uint8_t* data, size_t len) {
  uint8_t txt_type = data[4];
  if (type == PAYLOAD_TYPE_GRP_TXT && len > 5 && (txt_type >> 2) == 0) {  // 0 = plain text msg
    uint32_t timestamp;
    memcpy(&timestamp, data, 4);

    // len can be > original length, but 'text' will be padded with zeroes
    data[len] = 0; // need to make a C string again, with null terminator

    // notify UI  of this new message
    onChannelMessageRecv(channel, packet->isRouteFlood() ? packet->path_len : -1, timestamp, (const char *) &data[5]);  // let UI know
  }
}

mesh::Packet* BaseChatMesh::composeMsgPacket(const ContactInfo& recipient, uint8_t attempt, const char *text, uint32_t& expected_ack) {
  int text_len = strlen(text);
  if (text_len > MAX_TEXT_LEN) return NULL;

  uint8_t temp[5+MAX_TEXT_LEN+1];
  uint32_t timestamp = getRTCClock()->getCurrentTime();
  memcpy(temp, &timestamp, 4);   // mostly an extra blob to help make packet_hash unique
  temp[4] = (attempt & 3);
  memcpy(&temp[5], text, text_len + 1);

  // calc expected ACK reply
  mesh::Utils::sha256((uint8_t *)&expected_ack, 4, temp, 5 + text_len, self_id.pub_key, PUB_KEY_SIZE);

  return createDatagram(PAYLOAD_TYPE_TXT_MSG, recipient.id, recipient.shared_secret, temp, 5 + text_len);
}

int  BaseChatMesh::sendMessage(const ContactInfo& recipient, uint8_t attempt, const char* text, uint32_t& expected_ack) {
  mesh::Packet* pkt = composeMsgPacket(recipient, attempt, text, expected_ack);
  if (pkt == NULL) return MSG_SEND_FAILED;

  uint32_t t = _radio->getEstAirtimeFor(pkt->payload_len + pkt->path_len + 2);

  int rc;
  if (recipient.out_path_len < 0) {
    sendFlood(pkt);
    txt_send_timeout = futureMillis(calcFloodTimeoutMillisFor(t));
    rc = MSG_SEND_SENT_FLOOD;
  } else {
    sendDirect(pkt, recipient.out_path, recipient.out_path_len);
    txt_send_timeout = futureMillis(calcDirectTimeoutMillisFor(t, recipient.out_path_len));
    rc = MSG_SEND_SENT_DIRECT;
  }
  return rc;
}

void BaseChatMesh::resetPathTo(ContactInfo& recipient) {
  if (recipient.out_path_len >= 0) {
    recipient.out_path_len = -1;
  }
}

static ContactInfo* table;  // pass via global :-(

static int cmp_adv_timestamp(const void *a, const void *b) {
  int a_idx = *((int *)a);
  int b_idx = *((int *)b);
  if (table[b_idx].last_advert_timestamp > table[a_idx].last_advert_timestamp) return 1;
  if (table[b_idx].last_advert_timestamp < table[a_idx].last_advert_timestamp) return -1;
  return 0;
}

void BaseChatMesh::scanRecentContacts(int last_n, ContactVisitor* visitor) {
  for (int i = 0; i < num_contacts; i++) {  // sort the INDEXES into contacts[]
    sort_array[i] = i;
  }
  table = contacts; // pass via global *sigh* :-(
  qsort(sort_array, num_contacts, sizeof(sort_array[0]), cmp_adv_timestamp);

  if (last_n == 0) {
    last_n = num_contacts;   // scan ALL
  } else {
    if (last_n > num_contacts) last_n = num_contacts;
  }
  for (int i = 0; i < last_n; i++) {
    visitor->onContactVisit(contacts[sort_array[i]]);
  }
}

ContactInfo* BaseChatMesh::searchContactsByPrefix(const char* name_prefix) {
  int len = strlen(name_prefix);
  for (int i = 0; i < num_contacts; i++) {
    auto c = &contacts[i];
    if (memcmp(c->name, name_prefix, len) == 0) return c;
  }
  return NULL;  // not found
}

bool BaseChatMesh::addContact(const ContactInfo& contact) {
  if (num_contacts < MAX_CONTACTS) {
    auto dest = &contacts[num_contacts++];
    *dest = contact;

    // calc the ECDH shared secret (just once for performance)
    self_id.calcSharedSecret(dest->shared_secret, contact.id);

    return true;  // success
  }
  return false;
}

#ifdef MAX_GROUP_CHANNELS
#include <base64.hpp>

mesh::GroupChannel* BaseChatMesh::addChannel(const char* psk_base64) {
  if (num_channels < MAX_GROUP_CHANNELS) {
    auto dest = &channels[num_channels];

    memset(dest->secret, 0, sizeof(dest->secret));
    int len = decode_base64((unsigned char *) psk_base64, strlen(psk_base64), dest->secret);
    if (len == 32 || len == 16) {
      mesh::Utils::sha256(dest->hash, sizeof(dest->hash), dest->secret, len);
      num_channels++;
      return dest;
    }
  }
  return NULL;
}
#else
mesh::GroupChannel* BaseChatMesh::addChannel(const char* psk_base64) {
  return NULL;  // not supported
}
#endif

bool ContactsIterator::hasNext(const BaseChatMesh* mesh, ContactInfo& dest) {
  if (next_idx >= mesh->num_contacts) return false;

  dest = mesh->contacts[next_idx++];
  return true;
}

void BaseChatMesh::loop() {
  Mesh::loop();

  if (txt_send_timeout && millisHasNowPassed(txt_send_timeout)) {
    // failed to get an ACK
    onSendTimeout();
    txt_send_timeout = 0;
  }
}