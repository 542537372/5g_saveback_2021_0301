/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this file
 * except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.openairinterface.org/?page_id=698
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

#include "nr_pdcp_entity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nr_pdcp_security_nea2.h"
#include "nr_pdcp_sdu.h"

#include "LOG/log.h"

static void nr_pdcp_entity_recv_pdu(nr_pdcp_entity_t *entity,
                                    char *_buffer, int size)
{
  unsigned char    *buffer = (unsigned char *)_buffer;
  nr_pdcp_sdu_t    *sdu;
  int              rcvd_sn;
  uint32_t         rcvd_hfn;
  uint32_t         rcvd_count;
  int              header_size;
  int              integrity_size;
  int              rx_deliv_sn;
  uint32_t         rx_deliv_hfn;

  if (size < 1) {
    LOG_E(PDCP, "bad PDU received (size = %d)\n", size);
    return;
  }

  if (!(buffer[0] & 0x80)) {
    LOG_E(PDCP, "%s:%d:%s: fatal\n", __FILE__, __LINE__, __FUNCTION__);
    exit(1);
  }

  if (entity->sn_size == 12) {
    rcvd_sn = (((unsigned char)buffer[0] & 0xf) <<  8) |
                (unsigned char)buffer[1];
    header_size = 2;
  } else {
    rcvd_sn = (((unsigned char)buffer[0] & 0x3) << 16) |
               ((unsigned char)buffer[1]        <<  8) |
                (unsigned char)buffer[2];
    header_size = 3;
  }

  integrity_size = 0;

  if (size < header_size + integrity_size + 1) {
    LOG_E(PDCP, "bad PDU received (size = %d)\n", size);
    return;
  }

  rx_deliv_sn  = entity->rx_deliv & entity->sn_max;
  rx_deliv_hfn = (entity->rx_deliv >> entity->sn_size) & ~entity->sn_max;

  if (rcvd_sn < rx_deliv_sn - entity->window_size) {
    rcvd_hfn = rx_deliv_hfn + 1;
  } else if (rcvd_sn >= rx_deliv_sn + entity->window_size) {
    rcvd_hfn = rx_deliv_hfn - 1;
  } else {
    rcvd_hfn = rx_deliv_hfn;
  }

  rcvd_count = (rcvd_hfn << entity->sn_size) | rcvd_sn;

  if (entity->has_ciphering)
    entity->cipher(entity->security_context,
                   (unsigned char *)buffer+header_size, size-header_size,
                   entity->rb_id, rcvd_count, entity->is_gnb ? 0 : 1);

  if (rcvd_count < entity->rx_deliv
      || nr_pdcp_sdu_in_list(entity->rx_list, rcvd_count)) {
    LOG_D(PDCP, "discard NR PDU rcvd_count=%d\n", rcvd_count);
    return;
  }

  sdu = nr_pdcp_new_sdu(rcvd_count,
                        (char *)buffer + header_size,
                        size - header_size - integrity_size);
  entity->rx_list = nr_pdcp_sdu_list_add(entity->rx_list, sdu);
  entity->rx_size += size-header_size;

  if (rcvd_count >= entity->rx_next) {
    entity->rx_next = rcvd_count + 1;
  }

  /* TODO(?): out of order delivery */

  if (rcvd_count == entity->rx_deliv) {
    /* deliver all SDUs starting from rx_deliv up to discontinuity or end of list */
    uint32_t count = entity->rx_deliv;
    while (entity->rx_list != NULL && count == entity->rx_list->count) {
      nr_pdcp_sdu_t *cur = entity->rx_list;
      entity->deliver_sdu(entity->deliver_sdu_data, entity,
                          cur->buffer, cur->size);
      entity->rx_list = cur->next;
      entity->rx_size -= cur->size;
      nr_pdcp_free_sdu(cur);
      count++;
    }
    entity->rx_deliv = count;
  }

  if (entity->t_reordering_start != 0 && entity->rx_deliv > entity->rx_reord) {
    /* stop and reset t-Reordering */
    entity->t_reordering_start = 0;
  }

  if (entity->t_reordering_start == 0 && entity->rx_deliv < entity->rx_next) {
    entity->rx_reord = entity->rx_next;
    entity->t_reordering_start = entity->t_current;
  }
}

static void nr_pdcp_entity_recv_sdu(nr_pdcp_entity_t *entity,
                                    char *buffer, int size, int sdu_id)
{
  uint32_t count;
  int      sn;
  int      header_size;
  char     buf[size+3+4];

  count = entity->tx_next;
  sn = entity->tx_next & entity->sn_max;

  if (entity->sn_size == 12) {
    buf[0] = 0x80 | ((sn >> 8) & 0xf);
    buf[1] = sn & 0xff;
    header_size = 2;
  } else {
    buf[0] = 0x80 | ((sn >> 16) & 0x3);
    buf[1] = (sn >> 8) & 0xff;
    buf[2] = sn & 0xff;
    header_size = 3;
  }

  memcpy(buf+header_size, buffer, size);

  if (entity->has_ciphering)
    entity->cipher(entity->security_context,
                   (unsigned char *)buf+header_size, size,
                   entity->rb_id, count, entity->is_gnb ? 1 : 0);

  entity->tx_next++;

  entity->deliver_pdu(entity->deliver_pdu_data, entity, buf,
                      size+header_size, sdu_id);
}

static void nr_pdcp_entity_set_integrity_key(nr_pdcp_entity_t *entity,
                                             char *key)
{
  memcpy(entity->integrity_key, key, 16);
}

static void check_t_reordering(nr_pdcp_entity_t *entity)
{
  uint32_t count;

  if (entity->t_reordering_start == 0
      || entity->t_current <= entity->t_reordering_start + entity->t_reordering)
    return;

  /* stop timer */
  entity->t_reordering_start = 0;

  /* deliver all SDUs with count < rx_reord */
  while (entity->rx_list != NULL && entity->rx_list->count < entity->rx_reord) {
    nr_pdcp_sdu_t *cur = entity->rx_list;
    entity->deliver_sdu(entity->deliver_sdu_data, entity,
                        cur->buffer, cur->size);
    entity->rx_list = cur->next;
    entity->rx_size -= cur->size;
    nr_pdcp_free_sdu(cur);
  }

  /* deliver all SDUs starting from rx_reord up to discontinuity or end of list */
  count = entity->rx_reord;
  while (entity->rx_list != NULL && count == entity->rx_list->count) {
    nr_pdcp_sdu_t *cur = entity->rx_list;
    entity->deliver_sdu(entity->deliver_sdu_data, entity,
                        cur->buffer, cur->size);
    entity->rx_list = cur->next;
    entity->rx_size -= cur->size;
    nr_pdcp_free_sdu(cur);
    count++;
  }

  entity->rx_deliv = count;

  if (entity->rx_deliv < entity->rx_next) {
    entity->rx_reord = entity->rx_next;
    entity->t_reordering_start = entity->t_current;
  }
}

void nr_pdcp_entity_set_time(struct nr_pdcp_entity_t *entity, uint64_t now)
{
  entity->t_current = now;

  check_t_reordering(entity);
}

void nr_pdcp_entity_delete(nr_pdcp_entity_t *entity)
{
  nr_pdcp_sdu_t *cur = entity->rx_list;
  while (cur != NULL) {
    nr_pdcp_sdu_t *next = cur->next;
    nr_pdcp_free_sdu(cur);
    cur = next;
  }
  if (entity->free_security != NULL)
    entity->free_security(entity->security_context);
  free(entity);
}

nr_pdcp_entity_t *new_nr_pdcp_entity(
    nr_pdcp_entity_type_t type,
    int is_gnb, int rb_id,
    void (*deliver_sdu)(void *deliver_sdu_data, struct nr_pdcp_entity_t *entity,
                        char *buf, int size),
    void *deliver_sdu_data,
    void (*deliver_pdu)(void *deliver_pdu_data, struct nr_pdcp_entity_t *entity,
                        char *buf, int size, int sdu_id),
    void *deliver_pdu_data,
    int sn_size,
    int t_reordering,
    int discard_timer,
    int ciphering_algorithm,
    int integrity_algorithm,
    unsigned char *ciphering_key,
    unsigned char *integrity_key)
{
  nr_pdcp_entity_t *ret;

  ret = calloc(1, sizeof(nr_pdcp_entity_t));
  if (ret == NULL) {
    LOG_E(PDCP, "%s:%d:%s: out of memory\n", __FILE__, __LINE__, __FUNCTION__);
    exit(1);
  }

  ret->type = type;

  ret->recv_pdu          = nr_pdcp_entity_recv_pdu;
  ret->recv_sdu          = nr_pdcp_entity_recv_sdu;
  ret->set_integrity_key = nr_pdcp_entity_set_integrity_key;
  ret->set_time          = nr_pdcp_entity_set_time;

  ret->delete = nr_pdcp_entity_delete;

  ret->deliver_sdu = deliver_sdu;
  ret->deliver_sdu_data = deliver_sdu_data;

  ret->deliver_pdu = deliver_pdu;
  ret->deliver_pdu_data = deliver_pdu_data;

  ret->rb_id         = rb_id;
  ret->sn_size       = sn_size;
  ret->t_reordering  = t_reordering;
  ret->discard_timer = discard_timer;

  ret->sn_max        = (1 << sn_size) - 1;
  ret->window_size   = 1 << (sn_size - 1);

  if (ciphering_key != NULL && ciphering_algorithm != 0) {
    if (ciphering_algorithm != 2) {
      LOG_E(PDCP, "FATAL: only nea2 supported for the moment\n");
      exit(1);
    }
    ret->has_ciphering = 1;
    ret->ciphering_algorithm = ciphering_algorithm;
    memcpy(ret->ciphering_key, ciphering_key, 16);

    ret->security_context = nr_pdcp_security_nea2_init(ciphering_key);
    ret->cipher = nr_pdcp_security_nea2_cipher;
    ret->free_security = nr_pdcp_security_nea2_free_security;
  }
  ret->is_gnb = is_gnb;

  if (integrity_key != NULL) {
    printf("%s:%d:%s: TODO\n", __FILE__, __LINE__, __FUNCTION__);
    exit(1);
  }

  return ret;
}
