#include "ssh_model.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>

void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

int main()
{
    try {
        const std::string text = "# keep\r\nHost bastion\r\n    HostName bastion.example.com\r\n    User ops\r\n    Port 2222\r\n\r\nHost *\r\n    ServerAliveInterval 60\r\n";
        const auto cfg = envh::ssh::parseConfig(text);
        require(cfg.connectionIndexes().size() == 2, "connection blocks parsed");
        const auto* bastion = cfg.connection(0);
        require(bastion && envh::ssh::displayName(*bastion) == "bastion", "alias parsed");
        require(bastion->fields.hostname == "bastion.example.com", "hostname parsed");
        require(envh::ssh::isLocked(*cfg.connection(1)), "wildcard block is locked");
        require(envh::ssh::serializeBlock(*bastion, cfg.eol()).find("Host bastion") != std::string::npos, "block serializes");
        require(envh::ssh::fuzzyScore("Bastion production", "bpr") > -1000000, "fuzzy subsequence matches");
        require(envh::ssh::fuzzyScore("Bastion production", "xyz") == -1000000, "fuzzy subsequence rejects");
        require(envh::ssh::fuzzyScore("bastion", "bas") > envh::ssh::fuzzyScore("backup-bastion", "bas"), "fuzzy score ranks prefix");
        const auto key = envh::ssh::parsePublicKey("ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIAABAgMEBQYHCAkKCwwNDg8QERITFBUWFxgZGhscHR4f me@host");
        require(key.ok && key.type == "ed25519", "public key parses");
        require(envh::ssh::agentSocketValue("auto", "env-sock") == R"(\\.\pipe\openssh-ssh-agent)", "auto selects the system OpenSSH agent pipe");
        require(envh::ssh::agentSocketValue("env", "env-sock") == "env-sock", "env selects the inherited agent socket");
        require(envh::ssh::agentSocketValue(R"(\\.\pipe\custom-agent)", "env-sock") == R"(\\.\pipe\custom-agent)", "custom agent path is preserved");
        const auto testArgs = envh::ssh::connectionTestArgs("C:\\ssh\\config", "bastion", 7);
        require(!testArgs.empty() && testArgs.back() == "exit", "connection test ends with exit");
        require(std::find(testArgs.begin(), testArgs.end(), "bastion") != testArgs.end(), "connection test uses the alias");
        require(std::find(testArgs.begin(), testArgs.end(), "BatchMode=yes") != testArgs.end(), "connection test never prompts");
        require(std::find(testArgs.begin(), testArgs.end(), "ConnectTimeout=7") != testArgs.end(), "connection test honours the timeout");
        const auto okOutcome = envh::ssh::testOutcome(0, "banner\n");
        require(okOutcome.ok && okOutcome.detail.empty(), "exit 0 is a passing test");
        const auto failOutcome = envh::ssh::testOutcome(255, "Pseudo-terminal will not be allocated\nPermission denied (publickey).\n");
        require(!failOutcome.ok && failOutcome.detail == "Permission denied (publickey).", "failure keeps the last line");
        const auto longOutcome = envh::ssh::testOutcome(255, std::string(2000, 'x'));
        require(longOutcome.detail.size() == 400, "failure detail is capped");
        std::cout << "ssh model: parsing and wildcard handling passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
