// =============================================================================
//  SerialMeshtasticRadio — stub bodies, NOT YET WIRED TO HARDWARE.
// =============================================================================
//
//  TODO: confirm P4-to-C6 transport before enabling.
//
//  Step 4 research finding (against managed_components/badgeteam__badge-bsp/
//  targets/tanmatsu/tanmatsu_hardware.h, lines 24-38):
//
//      // ESP32-C6 radio
//      #define BSP_SDIO_CLK   17
//      #define BSP_SDIO_CMD   16
//      #define BSP_SDIO_D0    18
//      #define BSP_SDIO_D1    19
//      #define BSP_SDIO_D2    20
//      #define BSP_SDIO_D3    21
//      #define BSP_SDIO_WIDTH 4   // 4-bit SDIO mode
//
//  The Tanmatsu BSP defines the P4↔C6 link as a 4-bit SDIO bus, not a UART.
//  There are no UART pins exposed for the C6 in the BSP header. The board
//  ships with the C6 running ESP-Hosted firmware (see managed_components/
//  nicolaielectronics__esp-hosted-tanmatsu/), which is Espressif's
//  Wi-Fi/BLE coprocessor protocol — not Meshtastic.
//
//  To turn this class into something real, exactly one of these has to
//  happen first (none of which we should commit to without product input):
//
//    (A) Replace ESP-Hosted on the C6 with Meshtastic firmware that exposes
//        a serial-over-SDIO endpoint, then implement send/receive here as
//        framed packets over an sdmmc_host channel.
//
//    (B) Keep ESP-Hosted on the C6 and tunnel Meshtastic frames over its
//        EPPP virtual link (managed_components/espressif__eppp_link/).
//
//    (C) Move the radio to an external pin-broken-out UART and add a new
//        BSP entry — likely a board respin.
//
//  Until one of those is decided, the methods below log via ESP_LOGW and
//  behave exactly like StubMeshtasticRadio. The original Step 4 spec was
//  "uart_driver_install on the correct UART port" — that path doesn't
//  apply on this board, so the call is intentionally absent rather than
//  guessed at. See the example skeleton in the `#if 0` block at the end
//  of this file for what a UART-based version would look like if a future
//  board ever exposes one.
//
// =============================================================================

#include "meshtastic_radio_serial.h"
#include "esp_log.h"
#include <inttypes.h>

static const char *TAG = "MMRadio";

SerialMeshtasticRadio::SerialMeshtasticRadio() = default;

SerialMeshtasticRadio::~SerialMeshtasticRadio()
{
    end();
}

bool SerialMeshtasticRadio::begin()
{
    if (initialised_) return true;
    ESP_LOGW(TAG,
             "SerialMeshtasticRadio::begin() — TRANSPORT NOT WIRED. "
             "Tanmatsu P4-to-C6 link is SDIO (BSP_SDIO_*), not UART. "
             "See block comment in meshtastic_radio_serial.cpp.");
    initialised_ = true;
    return true;
}

void SerialMeshtasticRadio::end()
{
    if (!initialised_) return;
    initialised_ = false;
}

bool SerialMeshtasticRadio::sendPacket(uint32_t dest, uint8_t channel,
                                       const uint8_t* payload, size_t len)
{
    (void)payload;
    ESP_LOGW(TAG,
             "serial(stub) send: dest=0x%08" PRIx32 " ch=%u len=%u "
             "(transport not wired — packet dropped)",
             (uint32_t)dest, (unsigned)channel, (unsigned)len);

    // Length-prefixed framing the eventual real implementation will use:
    //   uint8_t hdr[4] = {
    //       (uint8_t)(len),
    //       (uint8_t)(len >> 8),
    //       (uint8_t)(len >> 16),
    //       (uint8_t)(len >> 24),
    //   };
    //   transport_write(hdr, 4);
    //   transport_write(payload, len);
    //
    // TODO: wrap in a Meshtastic protobuf envelope (ToRadio / MeshPacket) —
    // see _refs_monster_mesh/monster_mesh/protobufs/meshtastic/mesh.proto.

    return false;
}

int SerialMeshtasticRadio::pollPackets(MeshPacketSimple* out, int max_count)
{
    (void)out;
    (void)max_count;
    return 0;
}

int SerialMeshtasticRadio::getNeighborCount()
{
    return 0;
}

bool SerialMeshtasticRadio::getNodeList(MeshNodeInfo* nodes, int max_nodes, int* count)
{
    (void)nodes;
    (void)max_nodes;
    if (count) *count = 0;
    return true;
}

// =============================================================================
//  Reference skeleton for a UART-based implementation. Intentionally inside
//  #if 0 — kept here so a future board that does expose a P4-to-C6 UART can
//  enable it by setting CONFIG_MM_RADIO_UART_* and flipping the guard.
// =============================================================================
#if 0
#include "driver/uart.h"

static constexpr uart_port_t MM_RADIO_UART = UART_NUM_2;
static constexpr int         MM_RADIO_TX_PIN  = -1;   // TODO: set from BSP
static constexpr int         MM_RADIO_RX_PIN  = -1;   // TODO: set from BSP
static constexpr int         MM_RADIO_BAUD    = 921600;
static constexpr size_t      MM_RADIO_RX_BUF  = 4096;

static void install_uart() {
    uart_config_t cfg = {};
    cfg.baud_rate = MM_RADIO_BAUD;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity    = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;

    uart_driver_install(MM_RADIO_UART, MM_RADIO_RX_BUF, 0, 0, nullptr, 0);
    uart_param_config(MM_RADIO_UART, &cfg);
    uart_set_pin(MM_RADIO_UART, MM_RADIO_TX_PIN, MM_RADIO_RX_PIN,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}
#endif
