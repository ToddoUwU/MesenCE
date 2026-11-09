#include "Core/Shared/MessageManager.h"
#include "Core/Shared/Emulator.h"
#include "Core/Shared/EmuSettings.h"
#include "LinuxGameController.h"

#include "libevdev/libevdev.h"
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <string>

//Which input devices are controllers. Matching any device with an ABS_X axis (the old rule) let absolute mice,
//touchscreens, pen tablets, controller touchpads/motion sensors and RGB/LED controllers take pad slots, so "Pad1" was
//often not a controller at all (e.g. Sunshine/Moonlight's virtual mouse, pen and touch devices, or ASRock's motherboard
//LED controller, which reports Select/Start buttons and 8 axes).

//Joystick/gamepad buttons, counted like udev does (a mouse whose buttons overflow into the joystick range counts none).
static int CountJoystickButtons(libevdev* device)
{
	if(!libevdev_has_event_type(device, EV_KEY) || libevdev_has_event_code(device, EV_KEY, BTN_JOYSTICK - 1)) {
		return 0;
	}

	int count = 0;
	for(unsigned int code = BTN_JOYSTICK; code < BTN_DIGI; code++) {
		count += libevdev_has_event_code(device, EV_KEY, code) ? 1 : 0;
	}
	for(unsigned int code = BTN_TRIGGER_HAPPY1; code <= BTN_TRIGGER_HAPPY40; code++) {
		count += libevdev_has_event_code(device, EV_KEY, code) ? 1 : 0;
	}
	for(unsigned int code = BTN_DPAD_UP; code <= BTN_DPAD_RIGHT; code++) {
		count += libevdev_has_event_code(device, EV_KEY, code) ? 1 : 0;
	}
	return count;
}

//udev's verdict (ID_INPUT_JOYSTICK in its database), the same source SDL and Steam go by. Besides its capability rules,
//udev applies a list of devices known to look like joysticks without being one (the ASRock LED controller is on it).
//Returns -1 when udev has no entry for the device (no udev, e.g. some containers).
static int GetUdevJoystickFlag(dev_t rdev)
{
	char path[64];
	snprintf(path, sizeof(path), "/run/udev/data/c%u:%u", major(rdev), minor(rdev));
	std::ifstream file(path);
	if(!file) {
		return -1;
	}

	std::string line;
	while(std::getline(file, line)) {
		if(line == "E:ID_INPUT_JOYSTICK=1") {
			return 1;
		}
	}
	return 0;
}

static bool IsGameController(libevdev* device, dev_t rdev)
{
	//A device without any button is no use as a pad. udev flags some of those as joysticks too (e.g. a keyboard's
	//"System Control" node that only has two axes), and they would take pad slots ahead of real controllers.
	int buttons = CountJoystickButtons(device);
	if(buttons == 0) {
		return false;
	}

	int udevFlag = GetUdevJoystickFlag(rdev);
	if(udevFlag >= 0) {
		return udevFlag == 1;
	}

	//No udev: a short version of its rule. Not a pointer (mouse, touchscreen, touchpad, pen) or motion sensor...
	if(libevdev_has_property(device, INPUT_PROP_POINTER) || libevdev_has_property(device, INPUT_PROP_DIRECT) ||
		libevdev_has_property(device, INPUT_PROP_ACCELEROMETER)) {
		return false;
	}
	for(unsigned int code : { BTN_TOUCH, BTN_TOOL_PEN, BTN_TOOL_FINGER, BTN_STYLUS, BTN_MOUSE }) {
		if(libevdev_has_event_code(device, EV_KEY, code)) {
			return false;
		}
	}

	//...not a keyboard (udev's "well-known keys")...
	int keyboardKeys = 0;
	for(unsigned int code : { KEY_LEFTCTRL, KEY_CAPSLOCK, KEY_NUMLOCK, KEY_INSERT, KEY_MUTE, KEY_CALC, KEY_FILE, KEY_MAIL, KEY_PLAYPAUSE, KEY_BRIGHTNESSDOWN }) {
		keyboardKeys += libevdev_has_event_code(device, EV_KEY, code) ? 1 : 0;
	}

	//...and at least two joystick buttons/axes in all.
	int axes = 0;
	for(unsigned int code = ABS_RX; code < ABS_PRESSURE; code++) {
		axes += libevdev_has_event_code(device, EV_ABS, code) ? 1 : 0;
	}
	return keyboardKeys < 4 && buttons + axes >= 2;
}

std::shared_ptr<LinuxGameController> LinuxGameController::GetController(Emulator* emu, int deviceID, bool logInformation)
{
	std::string deviceName = "/dev/input/event" + std::to_string(deviceID);
	struct stat buffer;
	if(stat(deviceName.c_str(), &buffer) == 0) {
		int fd = open(deviceName.c_str(), O_RDWR | O_NONBLOCK);
		if(fd < 0) {
			if(logInformation) {
				MessageManager::Log("[Input] " + deviceName + "  error: " + std::to_string(errno) + " " + strerror(errno));
			}
			return nullptr;
		}

		libevdev* device = nullptr;
		int rc = libevdev_new_from_fd(fd, &device);
		if(rc < 0) {
			if(logInformation) {
				MessageManager::Log("[Input] " + deviceName + "  error: " + std::to_string(errno) + " " + strerror(errno));
			}
			close(fd);
			return nullptr;
		}

		if(IsGameController(device, buffer.st_rdev)) {
			MessageManager::Log(std::string("[Input Connected] Name: ") + libevdev_get_name(device) + " Vendor: " + std::to_string(libevdev_get_id_vendor(device)) + " Product: " + std::to_string(libevdev_get_id_product(device)));
			return std::shared_ptr<LinuxGameController>(new LinuxGameController(emu, deviceID, fd, device));
		} else {
			if(logInformation) {
				MessageManager::Log(std::string("[Input] Device ignored (Not a gamepad) - Name: ") + libevdev_get_name(device) + " Vendor: " + std::to_string(libevdev_get_id_vendor(device)) + " Product: " + std::to_string(libevdev_get_id_product(device)));
			}
			close(fd);
		}
	}
	return nullptr;
}

LinuxGameController::LinuxGameController(Emulator* emu, int deviceID, int fileDescriptor, libevdev* device)
{
	_emu = emu;
	_deviceID = deviceID;
	_stopFlag = false;
	_device = device;
	_fd = fileDescriptor;
	memset(_axisDefaultValue, 0, sizeof(_axisDefaultValue));

	_rumbleEffect.reset(new ff_effect());
	memset(_rumbleEffect.get(), 0, sizeof(ff_effect));
	_rumbleEffect->type = FF_RUMBLE;
	_rumbleEffect->id = -1;
	_rumbleEffect->u.rumble.strong_magnitude = 0x8000;
	_rumbleEffect->u.rumble.weak_magnitude = 0x8000;
	_rumbleEffect->replay.length = 2000;
	_rumbleEffect->replay.delay = 0;

	int rc = ioctl(_fd, EVIOCSFF, _rumbleEffect.get());
	if(rc < 0) {
		MessageManager::Log("Could not initialize force feedback effect");
		_rumbleEffect.reset();
	}

	_eventThread = std::thread([=]() {
		int rc;
		bool calibrate = true;

		do {
			fd_set readSet;
			FD_ZERO(&readSet);
			FD_SET(_fd, &readSet);

			//Timeout after 0.1 seconds (to allow thread to be terminated quickly)
			timeval timeout;
			timeout.tv_sec = 0;
			timeout.tv_usec = 100000;

			rc = select((int)_fd + 1, &readSet, nullptr, nullptr, &timeout);
			if(rc) {
				do {
					struct input_event ev;
					rc = libevdev_next_event(_device, LIBEVDEV_READ_FLAG_NORMAL, &ev);
					if(rc == LIBEVDEV_READ_STATUS_SYNC) {
						while(rc == LIBEVDEV_READ_STATUS_SYNC) {
							rc = libevdev_next_event(_device, LIBEVDEV_READ_FLAG_SYNC, &ev);
						}
					} else if(rc == LIBEVDEV_READ_STATUS_SUCCESS) {
						//print_event(&ev);
					}
				} while(rc == LIBEVDEV_READ_STATUS_SYNC || rc == LIBEVDEV_READ_STATUS_SUCCESS);
			}

			if(rc != LIBEVDEV_READ_STATUS_SYNC && rc != LIBEVDEV_READ_STATUS_SUCCESS && rc != -EAGAIN && rc != EWOULDBLOCK) {
				//Device was disconnected
				MessageManager::Log("[Input Device] Disconnected");
				break;
			}

			if(calibrate) {
				std::this_thread::sleep_for(std::chrono::duration<int, std::milli>(100));
				Calibrate();
				calibrate = false;
			}
		} while(!_stopFlag);

		_disconnected = true;
	});
}

LinuxGameController::~LinuxGameController()
{
	_stopFlag = true;
	_eventThread.join();

	libevdev_free(_device);
	close(_fd);
}

void LinuxGameController::Calibrate()
{
	int axes[14] = { ABS_X, ABS_Y, ABS_Z, ABS_RX, ABS_RY, ABS_RZ, ABS_HAT0X, ABS_HAT0Y, ABS_HAT1X, ABS_HAT1Y, ABS_HAT2X, ABS_HAT2Y, ABS_HAT3X, ABS_HAT3Y };
	for(int axis : axes) {
		_axisDefaultValue[axis] = libevdev_get_event_value(_device, EV_ABS, axis);
		//std::cout << "center values: " << std::to_string(_axisDefaultValue[axis]) << std::endl;
	}
}

bool LinuxGameController::CheckAxis(unsigned int code, bool forPositive)
{
	double deadZoneRatio = _emu->GetSettings()->GetControllerDeadzoneRatio();
	int deadZoneNegative = (_axisDefaultValue[code] - libevdev_get_abs_minimum(_device, code)) * 0.400 * deadZoneRatio;
	int deadZonePositive = (libevdev_get_abs_maximum(_device, code) - _axisDefaultValue[code]) * 0.400 * deadZoneRatio;

	if(forPositive) {
		return libevdev_get_event_value(_device, EV_ABS, code) - _axisDefaultValue[code] > deadZonePositive;
	} else {
		return libevdev_get_event_value(_device, EV_ABS, code) - _axisDefaultValue[code] < -deadZoneNegative;
	}
}

bool LinuxGameController::CheckButton(int btn)
{
	return libevdev_get_event_value(_device, EV_KEY, btn) == 1;
}

bool LinuxGameController::IsButtonPressed(int buttonNumber)
{
	bool pressed = false;
	switch(buttonNumber) {
		case 0: pressed = CheckButton(BTN_A); break;
		case 1: pressed = CheckButton(BTN_B); break;
		case 2: pressed = CheckButton(BTN_C); break;
		case 3: pressed = CheckButton(BTN_X); break;
		case 4: pressed = CheckButton(BTN_Y); break;
		case 5: pressed = CheckButton(BTN_Z); break;
		case 6: pressed = CheckButton(BTN_TL); break;
		case 7: pressed = CheckButton(BTN_TR); break;
		case 8: pressed = CheckButton(BTN_TL2); break;
		case 9: pressed = CheckButton(BTN_TR2); break;
		case 10: pressed = CheckButton(BTN_SELECT); break;
		case 11: pressed = CheckButton(BTN_START); break;
		case 12: pressed = CheckButton(BTN_THUMBL); break;
		case 13: pressed = CheckButton(BTN_THUMBR); break;

		case 14: pressed = CheckAxis(ABS_X, true); break;
		case 15: pressed = CheckAxis(ABS_X, false); break;
		case 16: pressed = CheckAxis(ABS_Y, true); break;
		case 17: pressed = CheckAxis(ABS_Y, false); break;
		case 18: pressed = CheckAxis(ABS_Z, true); break;
		case 19: pressed = CheckAxis(ABS_Z, false); break;
		case 20: pressed = CheckAxis(ABS_RX, true); break;
		case 21: pressed = CheckAxis(ABS_RX, false); break;
		case 22: pressed = CheckAxis(ABS_RY, true); break;
		case 23: pressed = CheckAxis(ABS_RY, false); break;
		case 24: pressed = CheckAxis(ABS_RZ, true); break;
		case 25: pressed = CheckAxis(ABS_RZ, false); break;

		//PS3 controller maps the D-Pad to BTN_DPAD_x unlike PS4/Xbox controllers, so check those too
		case 26: pressed = CheckAxis(ABS_HAT0X, true) || CheckButton(BTN_DPAD_RIGHT); break;
		case 27: pressed = CheckAxis(ABS_HAT0X, false) || CheckButton(BTN_DPAD_LEFT); break;
		case 28: pressed = CheckAxis(ABS_HAT0Y, true) || CheckButton(BTN_DPAD_DOWN); break;
		case 29: pressed = CheckAxis(ABS_HAT0Y, false) || CheckButton(BTN_DPAD_UP); break;

		case 30: pressed = CheckAxis(ABS_HAT1X, true); break;
		case 31: pressed = CheckAxis(ABS_HAT1X, false); break;
		case 32: pressed = CheckAxis(ABS_HAT1Y, true); break;
		case 33: pressed = CheckAxis(ABS_HAT1Y, false); break;
		case 34: pressed = CheckAxis(ABS_HAT2X, true); break;
		case 35: pressed = CheckAxis(ABS_HAT2X, false); break;
		case 36: pressed = CheckAxis(ABS_HAT2Y, true); break;
		case 37: pressed = CheckAxis(ABS_HAT2Y, false); break;
		case 38: pressed = CheckAxis(ABS_HAT3X, true); break;
		case 39: pressed = CheckAxis(ABS_HAT3X, false); break;
		case 40: pressed = CheckAxis(ABS_HAT3Y, true); break;
		case 41: pressed = CheckAxis(ABS_HAT3Y, false); break;

		case 42: pressed = CheckButton(BTN_TRIGGER); break;
		case 43: pressed = CheckButton(BTN_THUMB); break;
		case 44: pressed = CheckButton(BTN_THUMB2); break;
		case 45: pressed = CheckButton(BTN_TOP); break;
		case 46: pressed = CheckButton(BTN_TOP2); break;
		case 47: pressed = CheckButton(BTN_PINKIE); break;
		case 48: pressed = CheckButton(BTN_BASE); break;
		case 49: pressed = CheckButton(BTN_BASE2); break;
		case 50: pressed = CheckButton(BTN_BASE3); break;
		case 51: pressed = CheckButton(BTN_BASE4); break;
		case 52: pressed = CheckButton(BTN_BASE5); break;
		case 53: pressed = CheckButton(BTN_BASE6); break;
		case 54: pressed = CheckButton(BTN_DEAD); break;
	}

	_enableForceFeedback |= pressed;

	return pressed;
}

optional<int16_t> LinuxGameController::GetAxisPosition(int axis)
{
	axis -= 55;

	unsigned int code;
	switch(axis) {
		default: return std::nullopt;
		case 0: code = ABS_Y; break;
		case 1: code = ABS_X; break;
		case 2: code = ABS_RY; break;
		case 3: code = ABS_RX; break;
		case 4: code = ABS_Z; break;
		case 5: code = ABS_RZ; break;
	}

	int min = libevdev_get_abs_minimum(_device, code);
	int max = libevdev_get_abs_maximum(_device, code);

	int value = libevdev_get_event_value(_device, EV_ABS, code);

	int range = max - min;
	int offset = value - min;
	double ratio = (double)offset / range;

	int16_t axisValue = (ratio - 0.5) * 2 * INT16_MAX;
	return axis & 0x01 ? axisValue : -axisValue;
}

void LinuxGameController::SetForceFeedback(uint16_t magnitudeRight, uint16_t magnitudeLeft)
{
	if(!_rumbleEffect || !_enableForceFeedback) {
		return;
	}

	_rumbleEffect->u.rumble.strong_magnitude = magnitudeLeft;
	_rumbleEffect->u.rumble.weak_magnitude = magnitudeRight;
	int rc = ioctl(_fd, EVIOCSFF, _rumbleEffect.get());
	if(rc < 0) {
		//MessageManager::Log("Could not update force feedback effect.");
		return;
	}

	struct input_event play = {};
	play.type = EV_FF;
	play.code = _rumbleEffect->id;
	play.value = 1;

	rc = write(_fd, (const void*)&play, sizeof(play));
	if(rc < 0) {
		//MessageManager::Log("Could not play force feedback effect.");
	}
}

bool LinuxGameController::IsDisconnected()
{
	return _disconnected;
}

int LinuxGameController::GetDeviceID()
{
	return _deviceID;
}

/*
static int print_event(struct input_event *ev)
{
	if (ev->type == EV_SYN)
		printf("Event: time %ld.%06ld, ++++++++++++++++++++ %s +++++++++++++++\n",
				ev->time.tv_sec,
				ev->time.tv_usec,
				libevdev_event_type_get_name(ev->type));
	else
		printf("Event: time %ld.%06ld, type %d (%s), code %d (%s), value %d\n",
			ev->time.tv_sec,
			ev->time.tv_usec,
			ev->type,
			libevdev_event_type_get_name(ev->type),
			ev->code,
			libevdev_event_code_get_name(ev->type, ev->code),
			ev->value);
	return 0;
}
*/
