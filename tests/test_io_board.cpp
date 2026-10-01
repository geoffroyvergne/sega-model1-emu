// I/O board shared RAM (MB8421 at 0xC00000) tests: byte-lane mapping,
// Virtua Racing's boot accesses, and the inputs published into it by the
// high-level I/O board stand-in (InputManager).

#include "test_framework.hpp"

#include "core/dual_port_ram.hpp"
#include "core/input_manager.hpp"
#include "core/io_board.hpp"
#include "core/motherboard.hpp"

#include <cstdint>
#include <memory>
#include <initializer_list>
#include <string>
#include <vector>

namespace {

using model1::DualPortRam;
using model1::InputManager;
using model1::Motherboard;

bool log_contains(const char* text)
{
    return model1_test::captured_log().find(text) != std::string::npos;
}

std::unique_ptr<Motherboard> make_board()
{
    auto board = std::make_unique<Motherboard>();
    board->reset();
    return board;
}

} // namespace

TEST_CASE(io_shared_ram_vr_boot_accesses_read_back)
{
    auto board = make_board();
    model1::Bus& bus = board->bus();

    // VR boot: "SEGA" at 0xC00034-0xC0003A, then 0x01 at 0xC00040 and read back.
    const uint8_t sega[] = {'S', 'E', 'G', 'A'};
    for (uint32_t i = 0; i < 4; ++i) {
        bus.write_word(0xC00034 + i * 2, sega[i]);
    }
    bus.write_byte(0xC00040, 0x01);
    CHECK_EQ(bus.read_byte(0xC00040), 0x01u);
    CHECK_EQ(board->io_shared_ram().main_read(0x1A), static_cast<uint32_t>('S'));
    CHECK_EQ(board->io_shared_ram().main_read(0x1D), static_cast<uint32_t>('A'));

    // Then a 128-word block at 0xC00200-0xC002FE: written, read back.
    for (uint32_t i = 0; i < 128; ++i) {
        bus.write_word(0xC00200 + i * 2, static_cast<uint16_t>(i ^ 0x5A));
    }
    bool all_match = true;
    for (uint32_t i = 0; i < 128; ++i) {
        all_match = all_match && bus.read_word(0xC00200 + i * 2) == ((i ^ 0x5A) & 0xFF);
    }
    CHECK(all_match);
    CHECK_EQ(board->io_shared_ram().board_read(0x100), 0x5Au); // the I/O board sees it too
    CHECK(!log_contains("unmapped"));
    CHECK(!log_contains("unknown I/O board offset"));
}

TEST_CASE(io_shared_ram_byte_lanes_and_window)
{
    auto board = make_board();
    model1::Bus& bus = board->bus();

    bus.write_word(0xC00100, 0x1234);  // only the low byte lane is wired
    CHECK_EQ(bus.read_word(0xC00100), 0x0034u);
    bus.write_byte(0xC00101, 0x99);    // high lane: dropped
    CHECK_EQ(bus.read_byte(0xC00101), 0u);
    CHECK_EQ(bus.read_byte(0xC00100), 0x34u);
    bus.write_long(0xC00104, 0xAABBCCDD); // two word cycles: bytes 0x82 and 0x83
    CHECK_EQ(board->io_shared_ram().main_read(0x82), 0xDDu);
    CHECK_EQ(board->io_shared_ram().main_read(0x83), 0xBBu);

    bus.write_byte(0xC00FFE, 0x7E); // last byte of the 2 KB RAM
    CHECK_EQ(board->io_shared_ram().main_read(DualPortRam::k_size - 1), 0x7Eu);
    bus.write_byte(0xC01000, 0x01); // just past the window
    CHECK(log_contains("unmapped write8 at 0x00C01000"));

    board->reset(); // power-on: RAM cleared, input ports republished
    CHECK_EQ(bus.read_byte(0xC00100), 0u);
}

TEST_CASE(io_inputs_published_into_shared_ram)
{
    auto board = make_board();
    model1::Bus& bus = board->bus();
    InputManager& inputs = board->inputs();

    CHECK_EQ(bus.read_word(0xC00000), 0x00FFu); // analog channel 0: nothing connected
    CHECK_EQ(bus.read_word(0xC00010), 0x00FFu); // system port idle (active low)
    CHECK_EQ(bus.read_word(0xC00012), 0x00FFu);
    CHECK_EQ(bus.read_word(0xC00016), 0x00FFu); // unused digital port

    inputs.set_input(InputManager::Input::Coin1, true);
    inputs.set_input(InputManager::Input::P1Left, true);
    CHECK_EQ(bus.read_byte(0xC00010), 0xFEu);
    CHECK_EQ(bus.read_byte(0xC00012), 0x7Fu);
    inputs.set_input(InputManager::Input::Coin1, false);
    CHECK_EQ(bus.read_byte(0xC00010), 0xFFu);

    // Game-owned bytes are not touched when the ports are refreshed.
    bus.write_byte(0xC00034, 'S');
    board->run_frame();
    CHECK_EQ(bus.read_byte(0xC00034), static_cast<uint32_t>('S'));

    // Lamps / coin meters: written by the game, read by the board.
    bus.write_word(0xC0001E, 0x0005);
    CHECK_EQ(inputs.lamps(), 0x05u);
}

TEST_CASE(io_board_acknowledges_game_commands_once_per_frame)
{
    // VR boot at 0xFE022C: posts a command at 0xC00040 (byte 0x20) and loops
    // until the I/O board clears it.
    auto board = make_board();
    model1::Bus& bus = board->bus();
    bus.write_byte(0xC00040, 0x01);
    CHECK_EQ(bus.read_byte(0xC00040), 0x01u); // pending until the board runs
    board->run_frame();
    CHECK_EQ(bus.read_byte(0xC00040), 0x00u);
    CHECK(log_contains("[I/O board] Command 0x01 acknowledged"));

    bus.write_byte(0xC00040, 0x02);
    board->inputs().service();
    CHECK_EQ(bus.read_byte(0xC00040), 0x00u);
    CHECK(log_contains("[I/O board] Command 0x02 acknowledged"));
}

TEST_CASE(io_inputs_virtua_racing_controls)
{
    // MAME's "vr" ports: view buttons on the system port (bits 5-7) and
    // player 1 bit 0, shifter on player 1 bits 4 / 5; the joystick
    // directions drive the analog wheel and pedals, not port bits.
    auto board = make_board();
    model1::Bus& bus = board->bus();
    InputManager& inputs = board->inputs();
    inputs.set_profile(InputManager::Profile::VirtuaRacing);
    using In = InputManager::Input;

    // Idle: centred wheel, released pedals, channel 3 unconnected, twice.
    for (uint32_t base : {0xC00000u, 0xC00008u}) {
        CHECK_EQ(bus.read_byte(base + 0), 0x80u);
        CHECK_EQ(bus.read_byte(base + 2), 0x30u);
        CHECK_EQ(bus.read_byte(base + 4), 0x30u);
        CHECK_EQ(bus.read_byte(base + 6), 0xFFu);
    }

    inputs.set_input(In::P1Button1, true); // VR1 (red)
    inputs.set_input(In::P1Button3, true); // VR3 (yellow)
    inputs.set_input(In::P1Button4, true); // VR4 (green)
    inputs.set_input(In::P1Button6, true); // shift up
    CHECK_EQ(bus.read_byte(0xC00010), 0x5Fu);
    CHECK_EQ(bus.read_byte(0xC00012), 0xDEu);
    inputs.set_input(In::P1Button6, false);
    inputs.set_input(In::P1Button5, true); // shift down
    CHECK_EQ(bus.read_byte(0xC00012), 0xEEu);

    // Steering and pedals are not digital bits.
    inputs.set_input(In::P1Left, true);
    inputs.set_input(In::P1Up, true);
    CHECK_EQ(bus.read_byte(0xC00012), 0xEEu);

    // Held keys move the wheel and the accelerator a step per frame...
    inputs.update_analog();
    CHECK_EQ(inputs.analog(0), 0x80u - InputManager::k_wheel_step);
    CHECK_EQ(inputs.analog(1), 0x30u + InputManager::k_pedal_step);
    CHECK_EQ(inputs.analog(2), 0x30u);
    for (int i = 0; i < 20; ++i) {
        inputs.update_analog();
    }
    CHECK_EQ(inputs.analog(0), InputManager::k_wheel_left);
    CHECK_EQ(inputs.analog(1), 0xFFu);
    CHECK_EQ(bus.read_byte(0xC00000), 0x00u); // published
    CHECK_EQ(bus.read_byte(0xC00002), 0xFFu);

    // ...and spring back when released; both directions held = centred.
    inputs.set_input(In::P1Up, false);
    inputs.set_input(In::P1Right, true);
    for (int i = 0; i < 20; ++i) {
        inputs.update_analog();
    }
    CHECK_EQ(inputs.analog(0), InputManager::k_wheel_centre);
    CHECK_EQ(inputs.analog(1), InputManager::k_pedal_released);
    inputs.set_input(In::P1Left, false);
    inputs.set_input(In::P1Down, true);
    for (int i = 0; i < 20; ++i) {
        inputs.update_analog();
    }
    CHECK_EQ(inputs.analog(0), InputManager::k_wheel_right);
    CHECK_EQ(inputs.analog(2), 0xFFu);

    // The real I/O board reads the same values through its ADC.
    board->io_board().write(0xC000, 2);
    uint8_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = static_cast<uint8_t>((value << 1) | (board->io_board().read(0xC000) & 1));
    }
    CHECK_EQ(value, 0xFFu);

    // Reset releases everything but keeps the profile.
    board->reset();
    CHECK_EQ(inputs.analog(0), InputManager::k_wheel_centre);
    CHECK(inputs.profile() == InputManager::Profile::VirtuaRacing);
}

TEST_CASE(io_inputs_virtua_fighter_has_no_analog_controls)
{
    auto board = make_board();
    InputManager& inputs = board->inputs();
    inputs.set_input(InputManager::Input::P1Left, true);
    inputs.set_input(InputManager::Input::P1Button4, true); // not wired
    inputs.update_analog();
    CHECK_EQ(inputs.analog(0), 0xFFu);
    CHECK_EQ(inputs.port_value(InputManager::Port::Player1), 0xFF7Fu);
    CHECK_EQ(inputs.port_value(InputManager::Port::System), 0xFFFFu);
}

// ---------------------------------------------------------------------------
// 93C46 serial EEPROM (I/O board), driven pin by pin
// ---------------------------------------------------------------------------

namespace {

// Clocks `bits` (MSB first) into the EEPROM, advancing time by 2 per clock.
void eeprom_send(model1::Eeprom93c46& eeprom, uint64_t& now, uint32_t bits, int count)
{
    for (int i = count - 1; i >= 0; --i) {
        eeprom.write_di(((bits >> i) & 1) != 0);
        eeprom.write_clk(true, ++now);
        eeprom.write_clk(false, ++now);
    }
}

void eeprom_command(model1::Eeprom93c46& eeprom, uint64_t& now, uint32_t start_opcode_address, int count)
{
    eeprom.write_cs(true, ++now);
    eeprom_send(eeprom, now, start_opcode_address, count);
}

uint16_t eeprom_read(model1::Eeprom93c46& eeprom, uint64_t& now, uint32_t address)
{
    eeprom_command(eeprom, now, 0b110 << 6 | address, 9); // start, READ, address
    CHECK(!eeprom.read_do(now));                          // dummy 0
    uint16_t value = 0;
    for (int i = 0; i < 16; ++i) {
        eeprom.write_clk(true, ++now);
        value = static_cast<uint16_t>((value << 1) | (eeprom.read_do(now) ? 1 : 0));
        eeprom.write_clk(false, ++now);
    }
    eeprom.write_cs(false, ++now);
    return value;
}

} // namespace

TEST_CASE(eeprom_write_protect_write_busy_ready_and_read)
{
    model1::Eeprom93c46 eeprom(model1::IoBoard::k_clock_hz);
    eeprom.reset();
    uint64_t now = 100;
    CHECK_EQ(eeprom_read(eeprom, now, 5), 0xFFFFu); // erased

    // WRITE while write-protected (power-on state): ignored.
    eeprom_command(eeprom, now, 0b101 << 6 | 5, 9);
    eeprom_send(eeprom, now, 0xBEEF, 16);
    eeprom.write_cs(false, ++now);
    CHECK_EQ(eeprom.word(5), 0xFFFFu);

    // EWEN (start, 00, 11xxxx), then WRITE word 5.
    eeprom_command(eeprom, now, 0b100110000, 9);
    eeprom.write_cs(false, ++now);
    CHECK(eeprom.write_enabled());
    eeprom_command(eeprom, now, 0b101 << 6 | 5, 9);
    eeprom_send(eeprom, now, 0xBEEF, 16);
    eeprom.write_cs(false, ++now);
    CHECK_EQ(eeprom.word(5), 0xBEEFu);

    // Busy (DO = 0) until the 1.75 ms programming time has passed.
    const uint64_t written = now - 2; // the 16th data bit's rising CLK edge
    eeprom.write_cs(true, ++now);
    CHECK(!eeprom.read_do(now));
    CHECK(!eeprom.read_do(written + 6999));
    CHECK(eeprom.read_do(written + 7000)); // 1.75 ms at 4 MHz
    eeprom.write_cs(false, ++now);
    now = written + 7000;
    CHECK_EQ(eeprom_read(eeprom, now, 5), 0xBEEFu);

    // ERASE word 5, then reset: write-protected again, contents kept.
    now += 1;
    eeprom_command(eeprom, now, 0b111 << 6 | 5, 9);
    eeprom.write_cs(false, ++now);
    CHECK_EQ(eeprom.word(5), 0xFFFFu);
    eeprom.reset();
    CHECK(!eeprom.write_enabled());
}

// ---------------------------------------------------------------------------
// I/O board: 315-5338A, ADC, and a small Z80 firmware
// ---------------------------------------------------------------------------

namespace {

struct BoardRig {
    std::unique_ptr<model1::DualPortRam> ram = std::make_unique<model1::DualPortRam>();
    std::unique_ptr<InputManager> inputs = std::make_unique<InputManager>(*ram);
    std::unique_ptr<model1::IoBoard> board = std::make_unique<model1::IoBoard>(*ram, *inputs);
    explicit BoardRig(std::initializer_list<uint8_t> firmware = {0x18, 0xFE}) // JR $
    {
        inputs->set_publishing(false);
        inputs->reset();
        const std::vector<uint8_t> rom(firmware);
        board->load_rom(rom);
        board->reset();
    }
};

} // namespace

TEST_CASE(io_board_io_controller_reaches_shared_ram_and_adc)
{
    BoardRig rig;
    model1::IoBoard& b = *rig.board;
    // Latch address 0x0123 (low byte, then high byte), write 0x5A there.
    b.write(0x800A, 0x23);
    b.write(0x8009, 0x00);
    b.write(0x800A, 0x01);
    b.write(0x8009, 0x01);
    b.write(0x800A, 0x5A);
    b.write(0x8009, 0x07);
    CHECK_EQ(rig.ram->main_read(0x123), 0x5Au);
    CHECK_EQ(b.read(0x800C), 0x5Au); // read back through the controller
    b.write(0x800A, 0x77);
    b.write(0x8009, 0x73);           // direct write to shared byte 3
    CHECK_EQ(rig.ram->main_read(3), 0x77u);
    CHECK_EQ(b.read(0x800D), 0x08u); // status
    CHECK_EQ(b.read(0x800B), 0x73u); // command read-back

    // Ports: B as input reads the system inputs; as output, the latch.
    b.write(0x8008, 0x02);
    rig.inputs->set_input(InputManager::Input::Coin1, true);
    CHECK_EQ(b.read(0x8001), 0xFEu);
    b.write(0x8001, 0x33);
    b.write(0x8008, 0x00);
    CHECK_EQ(b.read(0x8001), 0x33u);
    // Port A bit 0 selects the DIP switches (all off) on ports B-D.
    b.write(0x8008, 0x02);
    b.write(0x8000, 0x01);
    CHECK_EQ(b.read(0x8001), 0xFFu);

    rig.inputs->set_profile(InputManager::Profile::VirtuaRacing);
    // ADC: select channel 0 (wheel, 0x80), then read 8 bits on D0, MSB first.
    b.write(0xC000, 0);
    uint8_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = static_cast<uint8_t>((value << 1) | (b.read(0xC000) & 1));
    }
    CHECK_EQ(value, 0x80u);
}

TEST_CASE(io_board_firmware_copies_inputs_into_shared_ram)
{
    // A minimal firmware in the style of the real one: port B (system
    // inputs) copied to shared RAM byte 0x08 in a loop.
    // Byte 0x08 is beyond the 0x70-0x77 direct range: latch the address.
    BoardRig rig;
    const std::vector<uint8_t> firmware = {
        0x3E, 0x02, 0x32, 0x08, 0x80,   // LD A, 0x02 ; LD (0x8008), A
        0x3E, 0x08, 0x32, 0x0A, 0x80,   // LD A, 0x08 ; LD (0x800A), A
        0xAF, 0x32, 0x09, 0x80,         // XOR A ; LD (0x8009), A   (address low = 0x08)
        0x32, 0x0A, 0x80,               // LD (0x800A), A           (A = 0)
        0x3E, 0x01, 0x32, 0x09, 0x80,   // LD A, 1 ; LD (0x8009), A (address high = 0)
        0x3A, 0x01, 0x80,               // loop (22): LD A, (0x8001)
        0x32, 0x0A, 0x80,               //            LD (0x800A), A
        0x3E, 0x07, 0x32, 0x09, 0x80,   //            LD A, 7 ; LD (0x8009), A (write)
        0x18, 0xF1,                     //            JR loop
    };
    rig.board->load_rom(firmware);
    rig.board->reset();
    for (int i = 0; i < 40; ++i) {
        rig.board->step();
    }
    CHECK_EQ(rig.ram->main_read(0x08), 0xFFu);
    rig.inputs->set_input(InputManager::Input::Coin1, true);
    for (int i = 0; i < 10; ++i) {
        rig.board->step();
    }
    CHECK_EQ(rig.ram->main_read(0x08), 0xFEu);
    CHECK(!rig.board->cpu().is_halted());
    CHECK(!log_contains("unmapped"));
}
