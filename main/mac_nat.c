#include "mac_nat.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "mac_nat";

#define MB_ETH_HDR_LEN          14
#define MB_ETH_TYPE_IPV4        0x0800
#define MB_ETH_TYPE_ARP         0x0806
#define MB_ETH_TYPE_VLAN        0x8100
#define MB_ETH_TYPE_QINQ        0x88A8

#define MB_ARP_OFF_SHA          8
#define MB_ARP_OFF_SPA          14
#define MB_ARP_OFF_THA          18
#define MB_ARP_OFF_TPA          24
#define MB_ARP_LEN              28

typedef struct {
    mac_nat_entry_t entries[MB_MAC_NAT_MAX_ENTRIES];
} mac_nat_table_t;

static mac_nat_table_t s_table;
static SemaphoreHandle_t s_lock;
static bool s_initialized;

static uint32_t now_sec(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000ULL);
}

static uint32_t read_ipv4(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint16_t read_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static bool mac_is_unicast(const uint8_t mac[6])
{
    return (mac[0] & 0x01) == 0;
}

/* Dipanggil dengan lock dipegang. */
static mac_nat_entry_t *find_entry(uint32_t ip)
{
    for (size_t i = 0; i < MB_MAC_NAT_MAX_ENTRIES; i++) {
        if (s_table.entries[i].valid && s_table.entries[i].client_ip == ip) {
            return &s_table.entries[i];
        }
    }
    return NULL;
}

static void prune_expired(uint32_t now)
{
    for (size_t i = 0; i < MB_MAC_NAT_MAX_ENTRIES; i++) {
        mac_nat_entry_t *e = &s_table.entries[i];
        if (e->valid && (uint32_t)(now - e->last_seen_sec) > MB_MAC_NAT_TTL_SEC) {
            e->valid = false;
        }
    }
}

/* Dipanggil dengan lock dipegang. */
static void learn_entry(uint32_t ip, const uint8_t mac[6], uint32_t now)
{
    if (ip == 0) {
        return; /* DHCP Discover: belum punya alamat, jangan cemari tabel. */
    }

    mac_nat_entry_t *e = find_entry(ip);
    if (e == NULL) {
        mac_nat_entry_t *oldest = NULL;
        for (size_t i = 0; i < MB_MAC_NAT_MAX_ENTRIES; i++) {
            mac_nat_entry_t *c = &s_table.entries[i];
            if (!c->valid) {
                e = c;
                break;
            }
            if (oldest == NULL ||
                (int32_t)(c->last_seen_sec - oldest->last_seen_sec) < 0) {
                oldest = c;
            }
        }
        if (e == NULL) {
            e = oldest;
        }
        if (e == NULL) {
            return;
        }
        e->client_ip = ip;
        e->valid = true;
    }

    memcpy(e->client_mac, mac, 6);
    e->last_seen_sec = now;
}

/* Ekstrak IPv4 src/dst dari frame. Mengembalikan false bila bukan IPv4 utuh. */
static bool parse_ipv4(const uint8_t *frame, uint16_t len,
                       uint32_t *src_ip, uint32_t *dst_ip, uint8_t *proto)
{
    if (len < MB_ETH_HDR_LEN + 20) {
        return false;
    }
    const uint8_t *ip = frame + MB_ETH_HDR_LEN;
    if ((ip[0] >> 4) != 4) {
        return false;
    }
    size_t ihl = (size_t)(ip[0] & 0x0f) * 4U;
    if (ihl < 20 || len < MB_ETH_HDR_LEN + ihl) {
        return false;
    }
    if (src_ip) {
        *src_ip = read_ipv4(ip + 12);
    }
    if (dst_ip) {
        *dst_ip = read_ipv4(ip + 16);
    }
    if (proto) {
        *proto = ip[9];
    }
    return true;
}

esp_err_t mac_nat_init(void)
{
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(&s_table, 0, sizeof(s_table));
    s_initialized = true;
    ESP_LOGI(TAG, "MAC NAT siap (%d entri)", MB_MAC_NAT_MAX_ENTRIES);
    return ESP_OK;
}

void mac_nat_clear(void)
{
    if (!s_initialized) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(&s_table, 0, sizeof(s_table));
    xSemaphoreGive(s_lock);
}

esp_err_t mac_nat_translate_tx(uint8_t *eth_frame, uint16_t len,
                               const uint8_t spoof_mac[6])
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (eth_frame == NULL || spoof_mac == NULL || len < MB_ETH_HDR_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t ethertype = read_be16(eth_frame + 12);
    if (ethertype == MB_ETH_TYPE_VLAN || ethertype == MB_ETH_TYPE_QINQ) {
        /* shortcut: tag VLAN tidak didukung; frame diteruskan tanpa NAT. */
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint32_t now = now_sec();
    uint8_t *src_mac = eth_frame + 6;

    if (ethertype == MB_ETH_TYPE_ARP) {
        if (len < MB_ETH_HDR_LEN + MB_ARP_LEN) {
            return ESP_ERR_INVALID_ARG;
        }
        uint8_t *arp = eth_frame + MB_ETH_HDR_LEN;
        if (read_be16(arp) != 0x0001 || read_be16(arp + 2) != MB_ETH_TYPE_IPV4 ||
            arp[4] != 6 || arp[5] != 4) {
            return ESP_ERR_NOT_SUPPORTED;
        }

        uint32_t spa = read_ipv4(arp + MB_ARP_OFF_SPA);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        learn_entry(spa, arp + MB_ARP_OFF_SHA, now);
        xSemaphoreGive(s_lock);

        memcpy(arp + MB_ARP_OFF_SHA, spoof_mac, 6);
        memcpy(src_mac, spoof_mac, 6);
        return ESP_OK;
    }

    if (ethertype != MB_ETH_TYPE_IPV4) {
        return ESP_ERR_NOT_FOUND;
    }

    uint32_t src_ip;
    if (!parse_ipv4(eth_frame, len, &src_ip, NULL, NULL)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    learn_entry(src_ip, src_mac, now);
    xSemaphoreGive(s_lock);

    memcpy(src_mac, spoof_mac, 6);
    return ESP_OK;
}

esp_err_t mac_nat_translate_rx(uint8_t *eth_frame, uint16_t len)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (eth_frame == NULL || len < MB_ETH_HDR_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t ethertype = read_be16(eth_frame + 12);
    if (ethertype == MB_ETH_TYPE_VLAN || ethertype == MB_ETH_TYPE_QINQ) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint32_t now = now_sec();

    if (ethertype == MB_ETH_TYPE_ARP) {
        if (len < MB_ETH_HDR_LEN + MB_ARP_LEN) {
            return ESP_ERR_INVALID_ARG;
        }
        uint8_t *arp = eth_frame + MB_ETH_HDR_LEN;
        if (read_be16(arp) != 0x0001 || read_be16(arp + 2) != MB_ETH_TYPE_IPV4 ||
            arp[4] != 6 || arp[5] != 4) {
            return ESP_ERR_NOT_SUPPORTED;
        }

        uint32_t tpa = read_ipv4(arp + MB_ARP_OFF_TPA);
        mac_nat_entry_t match;
        bool found = false;

        xSemaphoreTake(s_lock, portMAX_DELAY);
        prune_expired(now);
        mac_nat_entry_t *e = find_entry(tpa);
        if (e != NULL) {
            match = *e;
            e->last_seen_sec = now;
            found = true;
        }
        xSemaphoreGive(s_lock);

        if (!found) {
            return ESP_ERR_NOT_FOUND;
        }
        memcpy(arp + MB_ARP_OFF_THA, match.client_mac, 6);
        memcpy(eth_frame, match.client_mac, 6);
        return ESP_OK;
    }

    if (ethertype != MB_ETH_TYPE_IPV4) {
        return ESP_ERR_NOT_FOUND;
    }

    /* Hanya IPv4 unicast: jangan sentuh broadcast/multicast. */
    if (!mac_is_unicast(eth_frame)) {
        return ESP_ERR_NOT_FOUND;
    }

    uint32_t dst_ip;
    if (!parse_ipv4(eth_frame, len, NULL, &dst_ip, NULL)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    mac_nat_entry_t match;
    bool found = false;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    prune_expired(now);
    mac_nat_entry_t *e = find_entry(dst_ip);
    if (e != NULL) {
        match = *e;
        e->last_seen_sec = now;
        found = true;
    }
    xSemaphoreGive(s_lock);

    if (!found) {
        return ESP_ERR_NOT_FOUND;
    }

    memcpy(eth_frame, match.client_mac, 6);
    return ESP_OK;
}
