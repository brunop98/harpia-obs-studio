#include "MacGlobalHotkeys.hpp"
#include "../../core/GlobalHotkeys.hpp"

#include <Carbon/Carbon.h>
#include <QHash>
#include <QSet>
#include <QString>

#include <atomic>
#include <utility>

namespace harpia {
namespace {

bool keypadCode(UInt16 code)
{
	switch (code) {
	case 65: case 67: case 69: case 71: case 75: case 76: case 78:
	case 81: case 82: case 83: case 84: case 85: case 86: case 87:
	case 88: case 89: case 91: case 92: case 95: return true;
	default: return false;
	}
}

bool keyCodeForCharacter(unsigned character, UInt16 *code)
{
	TISInputSourceRef source = TISCopyCurrentKeyboardLayoutInputSource();
	if (!source)
		return false;
	CFDataRef data = static_cast<CFDataRef>(TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData));
	if (!data) {
		CFRelease(source);
		source = TISCopyCurrentASCIICapableKeyboardLayoutInputSource();
		if (!source)
			return false;
		data = static_cast<CFDataRef>(TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData));
	}
	bool found = false;
	if (data) {
		const auto *layout = reinterpret_cast<const UCKeyboardLayout *>(CFDataGetBytePtr(data));
		for (UInt16 candidate = 0; candidate < 128; ++candidate) {
			// A number on the keypad must not silently stand in for a number
			// which the current layout cannot produce on the main keyboard.
			if (keypadCode(candidate))
				continue;
			UInt32 deadState = 0;
			UniChar text[4] = {};
			UniCharCount length = 0;
			const OSStatus status = UCKeyTranslate(layout, candidate, kUCKeyActionDisplay, 0,
				LMGetKbdType(), 1u << kUCKeyTranslateNoDeadKeysBit, &deadState, 4, &length, text);
			if (status == noErr && length == 1 && QChar(text[0]).toUpper().unicode() == character) {
				*code = candidate;
				found = true;
				break;
			}
		}
	}
	CFRelease(source);
	return found;
}

} // namespace

struct MacGlobalHotkeys::Impl {
	std::function<void(int)> triggered;
	std::function<void()> layoutChanged;
	QHash<int, EventHotKeyRef> keys;
	QSet<int> pressed;
	EventHandlerRef handler = nullptr;
	EventHandlerUPP handlerProc = nullptr;
	UInt32 signature = 0;
	bool observingLayout = false;

	Impl(std::function<void(int)> onTriggered, std::function<void()> onLayoutChanged)
		: triggered(std::move(onTriggered)), layoutChanged(std::move(onLayoutChanged))
	{
		static std::atomic<UInt32> nextSignature{0x48520000}; // unique within this process
		signature = nextSignature.fetch_add(1);
	}
	~Impl()
	{
		if (observingLayout)
			CFNotificationCenterRemoveObserver(CFNotificationCenterGetDistributedCenter(), this,
				kTISNotifySelectedKeyboardInputSourceChanged, nullptr);
		if (handler)
			RemoveEventHandler(handler);
		if (handlerProc)
			DisposeEventHandlerUPP(handlerProc);
		clear();
	}
	void clear()
	{
		for (EventHotKeyRef key : keys)
			UnregisterEventHotKey(key);
		keys.clear();
		pressed.clear();
	}
	static OSStatus handle(EventHandlerCallRef, EventRef event, void *context)
	{
		auto *self = static_cast<Impl *>(context);
		EventHotKeyID id = {};
		if (GetEventParameter(event, kEventParamDirectObject, typeEventHotKeyID, nullptr,
			sizeof(id), nullptr, &id) != noErr || id.signature != self->signature ||
		    !self->keys.contains(int(id.id)))
			return eventNotHandledErr;
		const int action = int(id.id);
		if (GetEventKind(event) == kEventHotKeyReleased) {
			self->pressed.remove(action);
		} else if (!self->pressed.contains(action)) {
			self->pressed.insert(action);
			const auto callback = self->triggered;
			if (callback)
				callback(action);
		}
		return noErr;
	}
	static void layoutNotification(CFNotificationCenterRef, void *context, CFStringRef,
				       const void *, CFDictionaryRef)
	{
		auto *self = static_cast<Impl *>(context);
		// Stop old physical bindings before asking Qt to rebind. Until the
		// queued refresh runs, its existing window shortcuts remain available.
		self->clear();
		const auto callback = self->layoutChanged;
		if (callback)
			callback();
	}
	bool install()
	{
		if (handler)
			return true;
		const EventTypeSpec events[] = {
			{kEventClassKeyboard, kEventHotKeyPressed},
			{kEventClassKeyboard, kEventHotKeyReleased}};
		handlerProc = NewEventHandlerUPP(&Impl::handle);
		if (!handlerProc)
			return false;
		if (InstallApplicationEventHandler(handlerProc, 2, events, this, &handler) != noErr) {
			DisposeEventHandlerUPP(handlerProc);
			handlerProc = nullptr;
			return false;
		}
		CFNotificationCenterAddObserver(CFNotificationCenterGetDistributedCenter(), this,
			&Impl::layoutNotification, kTISNotifySelectedKeyboardInputSourceChanged, nullptr,
			CFNotificationSuspensionBehaviorDeliverImmediately);
		observingLayout = true;
		return true;
	}
};

MacGlobalHotkeys::MacGlobalHotkeys(std::function<void(int)> triggered, std::function<void()> layoutChanged)
	: d_(std::make_unique<Impl>(std::move(triggered), std::move(layoutChanged)))
{
}

MacGlobalHotkeys::~MacGlobalHotkeys() = default;

bool MacGlobalHotkeys::bind(int id, const MacHotkey &hotkey)
{
	if (id <= 0)
		return false;
	if (EventHotKeyRef previous = d_->keys.take(id))
		UnregisterEventHotKey(previous);
	d_->pressed.remove(id);
	if (!hotkey.valid || !d_->install())
		return false;
	UInt16 code = UInt16(hotkey.keyCode);
	if (hotkey.character && !keyCodeForCharacter(hotkey.character, &code))
		return false;
	EventHotKeyRef registered = nullptr;
	const EventHotKeyID nativeId{d_->signature, UInt32(id)};
	if (RegisterEventHotKey(code, hotkey.modifiers, nativeId, GetApplicationEventTarget(), 0, &registered) != noErr)
		return false;
	d_->keys.insert(id, registered);
	return true;
}

void MacGlobalHotkeys::unbindAll()
{
	d_->clear();
}

} // namespace harpia
