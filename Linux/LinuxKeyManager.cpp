#include <algorithm>
#include <climits>
#include <unistd.h>
#include "LinuxKeyManager.h"
#include "LinuxGameController.h"
#include "Utilities/FolderUtilities.h"
#include "Shared/Emulator.h"
#include "Shared/KeyDefinitions.h"

LinuxKeyManager::LinuxKeyManager(Emulator* emu)
{
	_emu = emu;

	ResetKeyState();

	_keyDefinitions = KeyDefinition::GetSharedKeyDefinitions();

	// clang-format off
	vector<string> buttonNames = { 
		"A", "B", "C", "X", "Y", "Z", "L1", "R1", "L2", "R2", "Select", "Start", "L3", "R3", 
		"X+", "X-", "Y+", "Y-", "Z+", "Z-", 
		"X2+", "X2-", "Y2+", "Y2-", "Z2+", "Z2-", 
		"Right", "Left", "Down", "Up", 
		"Right 2", "Left 2", "Down 2", "Up 2", 
		"Right 3", "Left 3", "Down 3", "Up 3", 
		"Right 4", "Left 4", "Down 4", "Up 4",
		"Trigger", "Thumb", "Thumb2", "Top", "Top2",
		"Pinkie", "Base", "Base2", "Base3", "Base4",
		"Base5", "Base6", "Dead",
		"Y", "X", "Y2", "X2", "Z", "Z2"
	};
	// clang-format on

	for(int i = 0; i < 20; i++) {
		for(int j = 0; j < (int)buttonNames.size(); j++) {
			_keyDefinitions.push_back({ "Pad" + std::to_string(i + 1) + " " + buttonNames[j], (uint32_t)(LinuxKeyManager::BaseGamepadIndex + i * 0x100 + j) });
		}
	}

	for(KeyDefinition& keyDef : _keyDefinitions) {
		_keyNames[keyDef.keyCode] = keyDef.name;
		//First definition wins: the analog axis names at the end of the pad list ("Y", "X", "Y2", ...) reuse button names,
		//and overwriting made "PadN X"/"PadN Y" resolve to axis codes that never read as pressed (Xbox preset turbo keys).
		_keyCodes.emplace(keyDef.name, keyDef.keyCode);
	}

	//The default Xbox layout binds Select to "PadN Back" (its name on other platforms); on Linux that button is "PadN Select".
	for(int i = 0; i < 20; i++) {
		string pad = "Pad" + std::to_string(i + 1) + " ";
		_keyCodes.emplace(pad + "Back", _keyCodes[pad + "Select"]);
	}

	CheckForGamepads(true);

	_disableAllKeys = false;
	_stopUpdateDeviceThread = false;
	StartUpdateDeviceThread();
}

LinuxKeyManager::~LinuxKeyManager()
{
	_stopUpdateDeviceThread = true;
	_stopSignal.Signal();
	_updateDeviceThread.join();
}

void LinuxKeyManager::RefreshState()
{
	//TODO: NOT IMPLEMENTED YET
	//Only needed to detect poll controller input
}

bool LinuxKeyManager::IsKeyPressed(uint16_t key)
{
	if(_disableAllKeys || key == 0) {
		return false;
	}

	if(key >= LinuxKeyManager::BaseGamepadIndex) {
		uint8_t gamepadPort = (key - LinuxKeyManager::BaseGamepadIndex) / 0x100;
		uint8_t gamepadButton = (key - LinuxKeyManager::BaseGamepadIndex) % 0x100;
		if(_controllers.size() > gamepadPort) {
			return _controllers[gamepadPort]->IsButtonPressed(gamepadButton);
		}
	} else if(key < 0x205) {
		return _keyState[key] != 0;
	}
	return false;
}

optional<int16_t> LinuxKeyManager::GetAxisPosition(uint16_t key)
{
	if(key >= LinuxKeyManager::BaseGamepadIndex) {
		uint8_t port = (key - LinuxKeyManager::BaseGamepadIndex) / 0x100;
		uint8_t button = (key - LinuxKeyManager::BaseGamepadIndex) % 0x100;
		return _controllers[port]->GetAxisPosition(button);
	}
	return std::nullopt;
}

bool LinuxKeyManager::IsMouseButtonPressed(MouseButton button)
{
	return _keyState[LinuxKeyManager::BaseMouseButtonIndex + (int)button];
}

vector<uint16_t> LinuxKeyManager::GetPressedKeys()
{
	vector<uint16_t> pressedKeys;
	for(size_t i = 0; i < _controllers.size(); i++) {
		for(int j = 0; j <= 54; j++) {
			if(_controllers[i]->IsButtonPressed(j)) {
				pressedKeys.push_back(LinuxKeyManager::BaseGamepadIndex + i * 0x100 + j);
			}
		}
	}

	for(int i = 0; i < 0x205; i++) {
		if(_keyState[i]) {
			pressedKeys.push_back(i);
		}
	}
	return pressedKeys;
}

string LinuxKeyManager::GetKeyName(uint16_t key)
{
	auto keyDef = _keyNames.find(key);
	if(keyDef != _keyNames.end()) {
		return keyDef->second;
	}
	return "";
}

uint16_t LinuxKeyManager::GetKeyCode(string keyName)
{
	auto keyDef = _keyCodes.find(keyName);
	if(keyDef != _keyCodes.end()) {
		return keyDef->second;
	}
	return 0;
}

void LinuxKeyManager::UpdateDevices()
{
	//TODO: NOT IMPLEMENTED YET
	//Only needed to detect newly plugged in devices
}

void LinuxKeyManager::CheckForGamepads(bool logInformation)
{
	vector<int> connectedIDs;
	for(int i = _controllers.size() - 1; i >= 0; i--) {
		if(!_controllers[i]->IsDisconnected()) {
			connectedIDs.push_back(_controllers[i]->GetDeviceID());
		}
	}

	vector<int> newIDs;
	vector<string> files = FolderUtilities::GetFilesInFolder("/dev/input/", {}, false);
	for(size_t i = 0; i < files.size(); i++) {
		string filename = FolderUtilities::GetFilename(files[i], false);
		if(filename.find("event", 0) == 0) {
			int deviceId = 0;
			try {
				deviceId = std::stoi(filename.substr(5));
			} catch(std::exception e) {
				continue;
			}

			if(std::find(connectedIDs.begin(), connectedIDs.end(), deviceId) == connectedIDs.end()) {
				newIDs.push_back(deviceId);
			}
		}
	}

	//Number pads in the order they were connected. The directory listing has no useful order (the newest node usually
	//comes first), so which controller became Pad1 changed from run to run when several were present.
	std::sort(newIDs.begin(), newIDs.end(), [](int a, int b) {
		int inputA = GetInputDeviceNumber(a);
		int inputB = GetInputDeviceNumber(b);
		return inputA != inputB ? inputA < inputB : a < b;
	});

	for(int deviceId : newIDs) {
		std::shared_ptr<LinuxGameController> controller = LinuxGameController::GetController(_emu, deviceId, logInformation);
		if(controller) {
			_controllers.push_back(controller);
		}
	}
}

int LinuxKeyManager::GetInputDeviceNumber(int eventId)
{
	//The kernel numbers input devices (the "inputN" parent of /dev/input/eventX) with a counter that only goes up,
	//unlike event numbers, which get reused. Sorting by it lists devices in connection order.
	char target[512] = {};
	string link = "/sys/class/input/event" + std::to_string(eventId) + "/device";
	ssize_t len = readlink(link.c_str(), target, sizeof(target) - 1);
	if(len > 0) {
		string path(target, len);
		size_t pos = path.rfind("input");
		if(pos != string::npos) {
			try {
				return std::stoi(path.substr(pos + 5));
			} catch(std::exception&) {
			}
		}
	}
	return INT_MAX;
}

void LinuxKeyManager::StartUpdateDeviceThread()
{
	_updateDeviceThread = std::thread([=]() {
		while(!_stopUpdateDeviceThread) {
			//Check for newly plugged in controllers every 5 secs
			vector<shared_ptr<LinuxGameController>> controllersToAdd;
			vector<int> indexesToRemove;
			for(int i = _controllers.size() - 1; i >= 0; i--) {
				if(_controllers[i]->IsDisconnected()) {
					indexesToRemove.push_back(i);
				}
			}

			CheckForGamepads(false);

			if(!indexesToRemove.empty() || !controllersToAdd.empty()) {
				_emu->Pause();
				for(int index : indexesToRemove) {
					_controllers.erase(_controllers.begin() + index);
				}
				for(std::shared_ptr<LinuxGameController> controller : controllersToAdd) {
					_controllers.push_back(controller);
				}
				_emu->Resume();
			}

			_stopSignal.Wait(5000);
		}
	});
}

bool LinuxKeyManager::SetKeyState(uint16_t scanCode, bool state)
{
	if(scanCode < 0x205 && _keyState[scanCode] != state) {
		_keyState[scanCode] = state;
		return true;
	}
	return false;
}

void LinuxKeyManager::ResetKeyState()
{
	memset(_keyState, 0, sizeof(_keyState));
}

void LinuxKeyManager::SetDisabled(bool disabled)
{
	_disableAllKeys = disabled;
}

void LinuxKeyManager::SetForceFeedback(uint16_t magnitudeRight, uint16_t magnitudeLeft)
{
	for(auto& controller : _controllers) {
		controller->SetForceFeedback(magnitudeRight, magnitudeLeft);
	}
}
