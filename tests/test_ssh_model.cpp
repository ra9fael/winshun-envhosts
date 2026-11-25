#include "ssh_model.h"
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
        std::cout << "ssh model: parsing and wildcard handling passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
