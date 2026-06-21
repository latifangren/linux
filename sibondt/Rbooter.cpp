#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <csignal>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <cstring>
#include <cstdlib> 

// --- Global Configuration ---
const std::string DEFAULT_SERIAL_PORT = "/dev/ttyUSB0";
const std::string ESC = "\x1b";
const std::string ENTER = "\r";
const std::string PROMPT = "Realtek>";
const std::string CHAIN_PROMPT = "RTD1619>";

const std::vector<std::string> STAGE1_COMMANDS = {
    "usb start",
    "fatload usb 0:1 0x01500000 u-boot.bin",
    "fatload usb 0:1 0x03000000 Image",
    "fatload usb 0:1 0x02100000 dtb/realtek/rtd1619-x1-prime-c.dtb",
    "fatload usb 0:1 0x02200000 uInitrd"
};

const std::vector<std::string> STAGE2_COMMANDS = {
    "setenv bootargs 'console=ttyS0,115200 earlycon=uart8250,mmio32,0x98007800 loglevel=8 ignore_loglevel rootwait rw root=LABEL=ROOTFS' nosmp",
    "booti 0x03000000 0x02200000:0x1b8498 0x02100000"
};

// --- Signal Handling (Ctrl+C) ---
volatile sig_atomic_t g_running = 1;
void signal_handler(int signum) {
    g_running = 0;
}

// --- Helper Functions ---
std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return std::tolower(c); });
    return s;
}

std::string read_and_print(int fd) {
    char c;
    ssize_t n = read(fd, &c, 1);
    if (n > 0) {
        std::cout << c << std::flush;
        return std::string(1, c);
    }
    return "";
}

std::string wait_for_text(int fd, const std::string& target, size_t buffer_limit = 32768) {
    std::string buf = "";
    std::string target_lower = to_lower(target);

    while (g_running) {
        std::string text = read_and_print(fd);
        if (text.empty()) continue;

        buf += text;

        if (buf.length() > buffer_limit) {
            buf = buf.substr(buf.length() - buffer_limit);
        }

        if (to_lower(buf).find(target_lower) != std::string::npos) {
            return buf;
        }
    }
    return buf;
}

void send_raw(int fd, const std::string& data) {
    write(fd, data.c_str(), data.length());
    fsync(fd);
}

void send_command_wait_prompt(int fd, const std::string& cmd, const std::string& prompt) {
    std::cout << "\n>>> " << cmd << std::flush;
    std::string payload = cmd + ENTER;
    write(fd, payload.c_str(), payload.length());
    fsync(fd);
    wait_for_text(fd, prompt);
}

void keep_logging(int fd, const std::string& target_log = "", int max_occurrences = 0) {
    std::string buf = "";
    std::string target_lower = to_lower(target_log);
    int occurrences = 0;

    while (g_running) {
        std::string text = read_and_print(fd);
        if (text.empty()) continue;

        if (!target_log.empty()) {
            buf += text;

            if (buf.length() > 4096) {
                buf = buf.substr(buf.length() - 1024);
            }

            if (to_lower(buf).find(target_lower) != std::string::npos) {
                occurrences++;
                buf = "";

                if (occurrences >= max_occurrences) {
                    std::cout << "\n[EXIT]" << std::flush;
                    break;
                }
            }
        }
    }
}

// --- Serial Port Setup ---
int setup_serial(const std::string& port) {
    int fd = open(port.c_str(), O_RDWR | O_NOCTTY | O_SYNC);
    if (fd < 0) {
        std::cerr << "\n[ERROR] Serial error: " << strerror(errno) << " (" << port << ")\n";
        return -1;
    }

    struct termios tty;
    if (tcgetattr(fd, &tty) != 0) {
        std::cerr << "\n[ERROR] tcgetattr error: " << strerror(errno) << std::endl;
        close(fd);
        return -1;
    }

    // Set baud rate to 115200
    cfsetospeed(&tty, B115200);
    cfsetispeed(&tty, B115200);

    // 8N1
    tty.c_cflag &= ~PARENB;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;
    
    // No hardware flow control
    tty.c_cflag &= ~CRTSCTS;
    tty.c_cflag |= CREAD | CLOCAL;

    // Non-canonical mode, non-echo
    tty.c_lflag = 0; 
    tty.c_oflag = 0; 
    
    // Disable software flow control & formatting
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);

    // Timeout config
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 1;

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        std::cerr << "\n[ERROR] tcsetattr error: " << strerror(errno) << std::endl;
        close(fd);
        return -1;
    }

    tcflush(fd, TCIOFLUSH);
    return fd;
}

// --- Main Program ---
int main() {
    std::signal(SIGINT, signal_handler);
    
    // Membersihkan layar terminal
    int result = system("clear");
    (void)result; // Mengabaikan nilai return untuk menghindari peringatan compiler
    
    // --- Header Formatting ---
    std::cout << "==============================\n";
    std::cout << "Realtek Tethered Kernel Booter\n";
    std::cout << "           @sib0ndt           \n";
    std::cout << "==============================\n";

    // --- Serial Port Selection ---
    std::string serial_port;
    std::cout << "(Default is " << DEFAULT_SERIAL_PORT << "): ";
    std::getline(std::cin, serial_port);

    // If user just presses Enter, use default port
    if (serial_port.empty()) {
        serial_port = DEFAULT_SERIAL_PORT;
    }

    std::cout << "Using : " << serial_port << "\n\n";

    int fd = setup_serial(serial_port);
    if (fd < 0) {
        return 1;
    }

    wait_for_text(fd, "welcome to lk/MP");
    send_raw(fd, ESC);
    
    wait_for_text(fd, "PCPU_FW_START");
    send_raw(fd, ENTER);
    
    wait_for_text(fd, PROMPT);

    for (const auto& cmd : STAGE1_COMMANDS) {
        if (!g_running) break;
        send_command_wait_prompt(fd, cmd, PROMPT);
    }

    if (g_running) {
        std::cout << "\n>>> chain 0x01500000" << std::flush;
        std::string chain_cmd = "chain 0x01500000\r";
        write(fd, chain_cmd.c_str(), chain_cmd.length());
        fsync(fd);

        wait_for_text(fd, CHAIN_PROMPT);
    }

    for (const auto& cmd : STAGE2_COMMANDS) {
        if (!g_running) break;
        
        if (cmd.find("booti ") == 0) {
            std::cout << "\n>>> " << cmd << std::flush;
            std::string booti_cmd = cmd + ENTER;
            write(fd, booti_cmd.c_str(), booti_cmd.length());
            fsync(fd);
        } else {
            send_command_wait_prompt(fd, cmd, CHAIN_PROMPT);
        }
    }

    if (g_running) {
        std::string target_message = "r8169 98016000.gmac eth0: link up";
        keep_logging(fd, target_message, 3);
    }

    if (!g_running) {
        std::cout << "\n[EXIT] Stopped by user.\n";
    }

    close(fd);
    return 0;
}

//g++ -O2 Rbooter.cpp -o Rbooter