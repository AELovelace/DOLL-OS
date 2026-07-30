//   config.h
//   build-time configuration for DollOS
#pragma once

//   Wi-Fi
//   First-boot defaults only. Once "wifi connect <ssid> <pass>" + "wifi save" has run,
//   the credentials in /wifi.cfg (LittleFS) win over these -- see loadWifiCredentials().
const char* STA_DEFAULT_SSID = "DollNet";
const char* STA_DEFAULT_PASSWORD = "WD10ears!";

//   FTP (FtpServer.ino) -- exposes the SD card over the network, as an alternative to
//   pulling the card or using USB MSC. Plaintext, LAN-only -- same posture as the rest
//   of this shell. Command port is the library default 21 (passive data on 50009).
//   Credentials must be < 16 chars (FTP_CRED_SIZE).
//   Connect from Explorer/FileZilla/WinSCP: ftp://<station-ip>/
const char* FTP_USER = "doll";
const char* FTP_PASS = "doll";

const char* MOTOKO_DEFAULT_BROKER = "192.168.44.4";
const int MOTOKO_DEFAULT_PORT = 1883;
const char* MOTOKO_CLIENT_ID = "MOTOKO-Cardputer";