// Copyright 2017 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "Core/HW/EXI_DeviceSlippi.h"

#include <array>
#include <unordered_map>
#include <stdexcept>

#include "SlippiLib/SlippiGame.h"
#include "Common/CommonFuncs.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/FileUtil.h"
#include "Common/StringUtil.h"
#include "wx/datetime.h"

CEXISlippi::CEXISlippi() {
	INFO_LOG(EXPANSIONINTERFACE, "EXI SLIPPI Constructor called.");
}

CEXISlippi::~CEXISlippi() {
	closeFile();
}

void CEXISlippi::configureCommands(u8* payload, u8 length) {
	for (int i = 1; i < length; i += 3) {
		// Go through the receive commands payload and set up other commands
		u8 commandByte = payload[i];
		u32 commandPayloadSize = payload[i + 1] << 8 | payload[i + 2];
		payloadSizes[commandByte] = commandPayloadSize;
	}
}

void CEXISlippi::writeToFile(u8* payload, u32 length, std::string fileOption) {
	std::vector<u8> dataToWrite;
	if (fileOption == "create") {
		// If the game sends over option 1 that means a file should be created
		createNewFile();

		// Start ubjson file and prepare the "raw" element that game
		// data output will be dumped into. The size of the raw output will
		// be initialized to 0 until all of the data has been received
		std::vector<u8> headerBytes(
			{ '{', 'U', 3, 'r', 'a', 'w', '[', '$', 'U', '#', 'l', 0, 0, 0, 0 }
		);
		dataToWrite.insert(dataToWrite.end(), headerBytes.begin(), headerBytes.end());

		// Used to keep track of how many bytes have been written to the file
		writenByteCount = 0;
	}

	// If no file, do nothing
	if (!m_file) {
		return;
	}

	// Add the payload to data to write
	dataToWrite.insert(dataToWrite.end(), payload, payload + length);
	writenByteCount += length;

	// If we are going to close the file, generate data to complete the UBJSON file
	if (fileOption == "close") {
		// This option indicates we are done sending over body
		std::vector<u8> closingBytes({ '}' });
		dataToWrite.insert(dataToWrite.end(), closingBytes.begin(), closingBytes.end());
	}

	// Write data to file
	bool result = m_file.WriteBytes(&dataToWrite[0], dataToWrite.size());
	if (!result) {
		ERROR_LOG(EXPANSIONINTERFACE, "Failed to write data to file.");
	}

	// If file should be closed, close it
	if (fileOption == "close") {
		// Write the number of bytes for the raw output
		u8 sizeByte0 = writenByteCount >> 24;
		u8 sizeByte1 = (writenByteCount & 0xFF0000) >> 16;
		u8 sizeByte2 = (writenByteCount & 0xFF00) >> 8;
		u8 sizeByte3 = writenByteCount & 0xFF;
		m_file.Seek(11, 0);
		std::vector<u8> sizeBytes({ sizeByte0, sizeByte1, sizeByte2, sizeByte3 });
		m_file.WriteBytes(&sizeBytes[0], sizeBytes.size());

		// Close file
		closeFile();
	}
}

void CEXISlippi::createNewFile() {
	if (m_file) {
		// If there's already a file open, close that one
		closeFile();
	}

	File::CreateDir("Slippi");
	std::string filepath = generateFileName();
	m_file = File::IOFile(filepath, "wb");
}

std::string CEXISlippi::generateFileName()
{
	std::string str = wxDateTime::Now().Format(wxT("%Y%m%dT%H%M%S"));
	return StringFromFormat("Slippi/Game_%s.slp", str.c_str());
}

void CEXISlippi::closeFile() {
	if (!m_file) {
		// If we have no file or payload is not game end, do nothing
		return;
	}

	// If this is the end of the game end payload, reset the file so that we create a new one
	m_file.Close();
	m_file = nullptr;
}

void CEXISlippi::loadFile(std::string path) {
	m_current_game = Slippi::SlippiGame::FromFile((std::string)path);
}

void CEXISlippi::prepareGameInfo() {
	// Since we are prepping new data, clear any existing data
	m_read_queue.clear();

	if (!m_current_game) {
		// Do nothing if we don't have a game loaded
		return;
	}

	Slippi::GameSettings* settings = m_current_game->GetSettings();

	// Build a word containing the stage and the presence of the characters
	u32 randomSeed = settings->randomSeed;
	m_read_queue.push_back(randomSeed);

	// This is kinda dumb but we need to handle the case where a player transforms
	// into sheik/zelda immediately. This info is not stored in the game info header
	// and so let's overwrite those values
	int player1Pos = 24; // This is the index of the first players character info
	std::array<uint32_t, Slippi::GAME_INFO_HEADER_SIZE> gameInfoHeader = settings->header;
	for (int i = 0; i < 4; i++) {
		// check if this player is actually in the game
		bool playerExists = m_current_game->DoesPlayerExist(i);
		if (!playerExists) {
			continue;
		}

		// check if the player is playing sheik or zelda
		uint8_t externalCharId = settings->players[i].characterId;
		if (externalCharId != 0x12 && externalCharId != 0x13) {
			continue;
		}

		// this is the position in the array that this player's character info is stored
		int pos = player1Pos + (9 * i);

		// here we have determined the player is playing sheik or zelda...
		// at this point let's overwrite the player's character with the one
		// that they are playing
		gameInfoHeader[pos] &= 0x00FFFFFF;
		gameInfoHeader[pos] |= externalCharId << 24;
	}

	// Write entire header to game
	for (int i = 0; i < Slippi::GAME_INFO_HEADER_SIZE; i++) {
		m_read_queue.push_back(gameInfoHeader[i]);
	}
}

void CEXISlippi::prepareFrameData(int32_t frameIndex, uint8_t port) {
	// Since we are prepping new data, clear any existing data
	m_read_queue.clear();

	if (!m_current_game) {
		// Do nothing if we don't have a game loaded
		return;
	}

	// Load the data from this frame into the read buffer
	uint8_t* a = (uint8_t*)&frameIndex;
	frameIndex = a[0] << 24 | a[1] << 16 | a[2] << 8 | a[3];
	bool frameExists = m_current_game->DoesFrameExist(frameIndex);
	if (!frameExists) {
		return;
	}

	Slippi::FrameData* frame = m_current_game->GetFrame(frameIndex);

	// Add random seed to the front of the response regardless of player
	m_read_queue.push_back(*(u32*)&frame->randomSeed);

	// Check if player exists
	if (!frame->players.count(port)) {
		return;
	}

	// Get data for this player
	Slippi::PlayerFrameData data = frame->players.at(port);

	// Add all of the inputs in order
	m_read_queue.push_back(*(u32*)&data.joystickX);
	m_read_queue.push_back(*(u32*)&data.joystickY);
	m_read_queue.push_back(*(u32*)&data.cstickX);
	m_read_queue.push_back(*(u32*)&data.cstickY);
	m_read_queue.push_back(*(u32*)&data.trigger);
	m_read_queue.push_back(data.buttons);
}

void CEXISlippi::prepareLocationData(int32_t frameIndex, uint8_t port) {
	// Since we are prepping new data, clear any existing data
	m_read_queue.clear();

	if (!m_current_game) {
		// Do nothing if we don't have a game loaded
		return;
	}

	// Load the data from this frame into the read buffer
	uint8_t* a = (uint8_t*)&frameIndex;
	frameIndex = a[0] << 24 | a[1] << 16 | a[2] << 8 | a[3];
	bool frameExists = m_current_game->DoesFrameExist(frameIndex);
	if (!frameExists) {
		return;
	}

	Slippi::FrameData* frame = m_current_game->GetFrame(frameIndex);
	if (!frame->players.count(port)) {
		return;
	}

	// Get data for this player
	Slippi::PlayerFrameData data = frame->players.at(port);

	// Add all of the inputs in order
	m_read_queue.push_back(*(u32*)&data.locationX);
	m_read_queue.push_back(*(u32*)&data.locationY);
	m_read_queue.push_back(*(u32*)&data.facingDirection);
}

void CEXISlippi::ImmWrite(u32 data, u32 size)
{
	//init();
	INFO_LOG(EXPANSIONINTERFACE, "EXI SLIPPI ImmWrite: %08x, size: %d", data, size);

	bool lookingForMessage = m_payload_type == CMD_UNKNOWN;
	if (lookingForMessage) {
		// If the size is not one, this can't be the start of a command
		if (size != 1) {
			return;
		}

		m_payload_type = data >> 24;

		// Attempt to get payload size for this command. If not found, don't do anything
		if (!payloadSizes.count(m_payload_type)) {
			m_payload_type = CMD_UNKNOWN;
			return;
		}
	}

	// Read and incremement our payload location
	m_payload_loc += size;

	// Add new data to payload
	for (u32 i = 0; i < size; i++) {
		int shiftAmount = 8 * (3 - i);
		u8 byte = 0xFF & (data >> shiftAmount);
		m_payload.push_back(byte);
	}

	// This section deals with saying we are done handling the payload
	// add one because we count the command as part of the total size
	u32 payloadSize = payloadSizes[m_payload_type];
	if (m_payload_type == CMD_RECEIVE_COMMANDS && m_payload_loc > 1) {
		// the receive commands command tells us exactly how long it is
		// this is to make adding new commands easier
		payloadSize = m_payload[1];
	}

	if (m_payload_loc >= payloadSize + 1) {
		// Handle payloads
		switch (m_payload_type) {
		case CMD_RECEIVE_COMMANDS:
			configureCommands(&m_payload[1], m_payload_loc - 1);
			writeToFile(&m_payload[0], m_payload_loc, "create");
			break;
		case CMD_RECEIVE_GAME_END:
			writeToFile(&m_payload[0], m_payload_loc, "close");
			break;
		case CMD_PREPARE_REPLAY:
			loadFile("Slippi/CurrentGame.slp");
			prepareGameInfo();
			break;
		case CMD_READ_FRAME:
			prepareFrameData(*(int32_t*)&m_payload[1], *(uint8_t*)&m_payload[5]);
			break;
		case CMD_GET_LOCATION:
			prepareLocationData(*(int32_t*)&m_payload[1], *(uint8_t*)&m_payload[5]);
			break;
		default:
			writeToFile(&m_payload[0], m_payload_loc, "");
			break;
		}

		// reset payload loc and type so we look for next command
		m_payload_loc = 0;
		m_payload_type = CMD_UNKNOWN;
		m_payload.clear();
	}
}

u32 CEXISlippi::ImmRead(u32 size)
{
	if (m_read_queue.empty()) {
		INFO_LOG(EXPANSIONINTERFACE, "EXI SLIPPI ImmRead: Empty");
		return 0;
	}

	u32 value = m_read_queue.front();
	m_read_queue.pop_front();

	INFO_LOG(EXPANSIONINTERFACE, "EXI SLIPPI ImmRead %08x", value);

	return value;
}

bool CEXISlippi::IsPresent() const
{
	return true;
}

void CEXISlippi::TransferByte(u8& byte)
{
}