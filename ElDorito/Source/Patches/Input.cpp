#define _USE_MATH_DEFINES
#include "Input.hpp"
#include <stack>
#include <cmath>

#include "../Patch.hpp"
#include "../Blam/BlamInput.hpp"
#include "../Blam/BlamObjects.hpp"
#include "../Modules/ModuleInput.hpp"
#include "../Console.hpp"
#include "../ElDorito.hpp"

using namespace Patches::Input;
using namespace Blam::Input;

namespace
{
	void UpdateInputHook();
	void ProcessUiInputHook();
	void QuickUpdateUiInputHook();
	void BlockInputHook(InputType type, bool blocked);
	void InitBindingsHook(BindingsTable *bindings);
	BindingsPreferences* PreferencesGetKeyBindingsHook(BindingsPreferences *result);
	void PreferencesSetKeyBindingsHook(BindingsPreferences newBindings);
	void GetDefaultBindingsHook(int type, BindingsTable *result);
	void GetKeyboardActionTypeHook();
	void ProcessKeyBindingsHook(const BindingsTable &bindings, ActionState *actions);
	void UpdateUiControllerInputHook(int a0);
	char GetControllerStateHook(int dwUserIndex, int a2, void *a3);
	DWORD SetControllerVibrationHook(int dwUserIndex, int a2, char a3);
	void LocalPlayerInputHook(int localPlayerIndex, uint32_t playerIndex, int a3, int a4, int a5, uint8_t* state);

	// Block/unblock input without acquiring or de-acquiring the mouse
	void QuickBlockInput();
	void QuickUnblockInput();

	std::stack<std::shared_ptr<InputContext>> contextStack;
	std::vector<DefaultInputHandler> defaultHandlers;
	bool contextDone = false;
	bool blockStates[eInputType_Count];

	std::vector<ConfigurableAction> settings;

	extern InputType actionInputTypes[eGameAction_KeyboardMouseCount];
}

namespace Patches
{
	namespace Input
	{
		void ApplyAll()
		{
			Hook(0x1129A0, UpdateInputHook).Apply();
			Hook(0x105CBA, ProcessUiInputHook, HookFlags::IsCall).Apply();
			Hook(0x106417, QuickUpdateUiInputHook, HookFlags::IsCall).Apply();
			Hook(0x234238, BlockInputHook, HookFlags::IsCall).Apply();
			Hook(0x20BF00, InitBindingsHook).Apply();
			Hook(0x10AB40, PreferencesGetKeyBindingsHook).Apply();
			Hook(0x10D040, PreferencesSetKeyBindingsHook).Apply();
			Hook(0x20C040, GetDefaultBindingsHook).Apply();
			Hook(0x20C4F6, GetKeyboardActionTypeHook).Apply();
			Hook(0x1128FB, GetControllerStateHook, HookFlags::IsCall).Apply();
			Hook(0x11298B, SetControllerVibrationHook, HookFlags::IsCall).Apply();
			Patch::NopFill(Pointer::Base(0x6A225B), 2); // Prevent the game from forcing certain binds on load
			Patch(0x6940E7, { 0x90, 0xE9 }).Apply(); // Disable custom UI input code
			Hook(0x695012, UpdateUiControllerInputHook, HookFlags::IsCall).Apply();
			Patch::NopFill(Pointer::Base(0x112613), 2); // Never clip the cursor to the window boundaries

			// Fix a bug in the keyboard input routine that screws up UI keys.
			// If an action has the "handled" flag set and has a secondary key
			// bound to it, then the tick count gets reset to 0 because the
			// secondary key won't be down and the action gets spammed.
			//
			// To fix it, we have to nop out every place where the flag is
			// checked, disable the code which updates the flag, and then
			// update it ourselves only after every key has been checked.
			uint32_t keyboardFixPointers[] = { 0x20C529, 0x20C558, 0x20C591, 0x20C5C2, 0x20C5FA, 0x20C63F };
			for (auto pointer : keyboardFixPointers)
				Patch::NopFill(Pointer::Base(pointer), 2);
			Patch(0x20C69F, { 0xEB }).Apply();
			Hook(0x20D980, ProcessKeyBindingsHook, HookFlags::IsCall).Apply();
			Hook(0x1D4C66, LocalPlayerInputHook, HookFlags::IsCall).Apply();
		}

		void PushContext(std::shared_ptr<InputContext> context)
		{
			if (contextStack.empty())
			{
				// Block all input, unacquiring the mouse in the process
				for (auto i = 0; i < eInputType_Count; i++)
					BlockInput(static_cast<InputType>(i), true);
			}
			else
			{
				// Deactivate the current context
				contextStack.top()->Deactivated();
			}

			// Push and activate the new context
			contextStack.push(context);
			context->Activated();
		}

		void RegisterDefaultInputHandler(DefaultInputHandler func)
		{
			defaultHandlers.push_back(func);
		}

		void SetKeyboardSettingsMenu(
			const std::vector<ConfigurableAction> &infantrySettings,
			const std::vector<ConfigurableAction> &vehicleSettings)
		{
			// The settings array needs to have infantry settings followed by
			// vehicle settings due to assumptions that the EXE makes
			settings.clear();
			settings.insert(settings.end(), infantrySettings.begin(), infantrySettings.end());
			settings.insert(settings.end(), vehicleSettings.begin(), vehicleSettings.end());

			// Patch the exe to point to the new menus
			auto infantryCount = infantrySettings.size();
			auto vehicleCount = vehicleSettings.size();
			uint32_t infantryPointers[] = { 0x398573, 0x3993CF, 0x39A42F, 0x39B4B0 };
			uint32_t vehiclePointers[] = { 0x39856D, 0x3993CA, 0x39A63A, 0x39A645, 0x39B4AB };
			uint32_t endPointers[] = { 0x39A84A };
			uint32_t infantryCountPointers[] = { 0x39857B, 0x3993BC, 0x39B495 };
			uint32_t vehicleCountPointers[] = { 0x398580, 0x3993C1, 0x39B49A };
			for (auto pointer : infantryPointers)
				Pointer::Base(pointer).Write<ConfigurableAction*>(&settings[0]);
			for (auto pointer : vehiclePointers)
				Pointer::Base(pointer).Write<ConfigurableAction*>(&settings[0] + infantryCount);
			for (auto pointer : endPointers)
				Pointer::Base(pointer).Write<ConfigurableAction*>(&settings[0] + settings.size());
			for (auto pointer : infantryCountPointers)
				Pointer::Base(pointer).Write<int>(static_cast<int>(infantryCount));
			for (auto pointer : vehicleCountPointers)
				Pointer::Base(pointer).Write<int>(static_cast<int>(vehicleCount));
		}
	}
}

namespace
{
	void PopContext()
	{
		contextStack.top()->Deactivated();
		contextStack.pop();
		if (!contextStack.empty())
		{
			// Activate the previous context
			contextStack.top()->Activated();
		}
		else
		{
			// Restore the game's input block states
			for (auto i = 0; i < eInputType_Count; i++)
				BlockInput(static_cast<InputType>(i), blockStates[i]);
		}
	}

	void QuickBlockInput()
	{
		memset(reinterpret_cast<bool*>(0x238DBEB), 1, eInputType_Count);
	}

	void QuickUnblockInput()
	{
		memset(reinterpret_cast<bool*>(0x238DBEB), 0, eInputType_Count);
	}

	void UpdateInputHook()
	{
		// If the current context is done, pop it off
		if (contextDone)
		{
			PopContext();
			contextDone = false;
		}

		if (!contextStack.empty())
		{
			// Tick the active context
			QuickUnblockInput();
			if (!contextStack.top()->GameInputTick())
				contextDone = true;
			QuickBlockInput();
		}
		else
		{
			// Run default handlers
			for (auto &&handler : defaultHandlers)
				handler();
		}
	}

	void UiInputTick()
	{
		if (contextStack.empty())
			return;

		// Tick the active context
		QuickUnblockInput();
		if (!contextStack.top()->UiInputTick())
			contextDone = true;
		QuickBlockInput();
	}

	void ProcessUiInputHook()
	{
		// Pump Windows messages (replaced function)
		typedef void(*PumpMessagesPtr)();
		auto PumpMessages = reinterpret_cast<PumpMessagesPtr>(0x508170);
		PumpMessages();

		UiInputTick();
	}

	void QuickUpdateUiInputHook()
	{
		// Quick pump Windows messages (replaced function)
		typedef void(*QuickPumpMessagesPtr)();
		auto QuickPumpMessages = reinterpret_cast<QuickPumpMessagesPtr>(0x42E940);
		QuickPumpMessages();

		UiInputTick();
	}

	void BlockInputHook(InputType type, bool blocked)
	{
		// If a context isn't active, then block input normally,
		// otherwise save the value for when the contexts are done
		if (contextStack.empty())
			BlockInput(type, blocked);
		blockStates[type] = blocked;
	}

	// Hook to initialize bindings with ModuleInput's values
	void InitBindingsHook(BindingsTable *bindings)
	{
		*bindings = *Modules::ModuleInput::GetBindings();
	}

	// Hook to redirect keybind preference reads to ModuleInput
	BindingsPreferences* PreferencesGetKeyBindingsHook(BindingsPreferences *result)
	{
		auto bindings = Modules::ModuleInput::GetBindings();
		memcpy(result->PrimaryKeys, bindings->PrimaryKeys, sizeof(result->PrimaryKeys));
		memcpy(result->PrimaryMouseButtons, bindings->PrimaryMouseButtons, sizeof(result->PrimaryMouseButtons));
		memcpy(result->SecondaryKeys, bindings->SecondaryKeys, sizeof(result->SecondaryKeys));
		memcpy(result->SecondaryMouseButtons, bindings->SecondaryMouseButtons, sizeof(result->SecondaryMouseButtons));
		return result;
	}

	// Hook to redirect keybind preference writes to ModuleInput
	void PreferencesSetKeyBindingsHook(BindingsPreferences newBindings)
	{
		auto bindings = Modules::ModuleInput::GetBindings();
		memcpy(bindings->PrimaryKeys, newBindings.PrimaryKeys, sizeof(bindings->PrimaryKeys));
		memcpy(bindings->PrimaryMouseButtons, newBindings.PrimaryMouseButtons, sizeof(bindings->PrimaryMouseButtons));
		memcpy(bindings->SecondaryKeys, newBindings.SecondaryKeys, sizeof(bindings->SecondaryKeys));
		memcpy(bindings->SecondaryMouseButtons, newBindings.SecondaryMouseButtons, sizeof(bindings->SecondaryMouseButtons));
		Modules::ModuleInput::UpdateBindings();
		Modules::CommandMap::Instance().ExecuteCommand("WriteConfig");
	}

	// Hook to prevent the game from resetting keybindings when we don't want it to
	void GetDefaultBindingsHook(int type, BindingsTable *result)
	{
		*result = *Modules::ModuleInput::GetBindings();
	}

	// Hook to get keyboard action types from the actionInputTypes array
	// instead of using hardcoded values
	__declspec(naked) void GetKeyboardActionTypeHook()
	{
		__asm
		{
			// ecx has the action index
			// eax needs to contain the type on return
			// ecx needs to be 0 on return
			mov eax, actionInputTypes[ecx * 4]
			xor ecx, ecx
			push 0x60C51E
			ret
		}
	}

	void ProcessKeyBindingsHook(const BindingsTable &bindings, ActionState *actions)
	{
		typedef void(*EngineProcessKeyBindingsPtr)(const BindingsTable &bindings, ActionState *actions);
		auto EngineProcessKeyBindings = reinterpret_cast<EngineProcessKeyBindingsPtr>(0x60C4A0);
		EngineProcessKeyBindings(bindings, actions);

		// Unset the "handled" flag for inactive actions
		for (auto i = 0; i < eGameAction_KeyboardMouseCount; i++)
		{
			if (actions[i].Ticks == 0)
				actions[i].Flags &= ~eActionStateFlagsHandled;
		}
	}

	void UpdateUiControllerInputHook(int a0)
	{
		typedef void(*UiUpdateControllerInputPtr)(int a0);
		auto UiUpdateControllerInput = reinterpret_cast<UiUpdateControllerInputPtr>(0xA93A50);
		typedef bool(*IsMainMenuPtr)();
		auto IsMainMenu = reinterpret_cast<IsMainMenuPtr>(0x531E90);
		typedef float(*UiGetTimeDeltaPtr)();
		auto UiGetTimeDelta = reinterpret_cast<UiGetTimeDeltaPtr>(0xA844E0);
		typedef void(*UpdateCharPlatformPtr)();
		auto UpdateCharPlatform = reinterpret_cast<UpdateCharPlatformPtr>(0xBB5F00);
		typedef void(*RotateCharPlatformPtr)(float timeDelta, float amount);
		auto RotateCharPlatform = reinterpret_cast<RotateCharPlatformPtr>(0xBB5DA0);

		UiUpdateControllerInput(a0);

		// Handle char_platform controls
		//
		// This spices things up a bit compared to the default implementation
		// by making the platform automatically rotate slowly until the player
		// manually rotates it.

		static auto firstRotate = true;
		static auto autoRotate = true;
		if (!IsMainMenu())
		{
			// char_platform is only on the main menu
			firstRotate = true;
			autoRotate = true;
			return;
		}
		if (firstRotate)
		{
			// Rotate it to the left a bit initially so that you can see the
			// front of your Spartan at the start
			RotateCharPlatform(1.0f, -0.25f);
			firstRotate = false;
			return;
		}
		auto leftAction = GetActionState(eGameActionUiLeft);
		auto rightAction = GetActionState(eGameActionUiRight);
		auto rotateLeft = (leftAction->Ticks != 0);
		auto rotateRight = (rightAction->Ticks != 0);
		auto rotateAmount = static_cast<int>(rotateRight) - static_cast<int>(rotateLeft);
		if (rotateAmount)
		{
			RotateCharPlatform(UiGetTimeDelta(), rotateAmount * 1.0f);
			autoRotate = false;
		}
		else if (autoRotate)
		{
			// Slowly rotate counterclockwise
			RotateCharPlatform(UiGetTimeDelta(), 0.025f);
		}
		else
		{
			UpdateCharPlatform();
		}
	}

	char GetControllerStateHook(int dwUserIndex, int a2, void *a3)
	{
		typedef char(*GetControllerStatePtr)(int dwUserIndex, int a2, void *a3);
		auto GetControllerState = reinterpret_cast<GetControllerStatePtr>(0x65EF60);
		auto val = GetControllerState(Modules::ModuleInput::Instance().VarInputControllerPort->ValueInt, a2, a3);

		//Invert right joystick
		if (Modules::ModuleInput::Instance().VarControllerInvertY->ValueInt)
		{
			auto rY = Pointer(a3)(0x3A).Read<short>();
			//Prevent an overflow
			if (rY == -32768)
				rY++;
			Pointer(a3)(0x3A).Write<short>(rY * -1);
		}

		return contextStack.empty() ? val : 0;
	}


	DWORD SetControllerVibrationHook(int dwUserIndex, int a2, char a3)
	{
		typedef char(*SetControllerVibrationPtr)(int dwUserIndex, int a2, char a3);
		auto SetControllerVibration = reinterpret_cast<SetControllerVibrationPtr>(0x65F220);

		return SetControllerVibration(Modules::ModuleInput::Instance().VarInputControllerPort->ValueInt, a2, a3);
	}

	void LocalPlayerInputHook(int localPlayerIndex, uint32_t playerIndex, int a3, int a4, int a5, uint8_t* state)
	{
		static auto LocalPlayerInputHook = (void(__cdecl*)(int localPlayerIndex, uint32_t playerIndex, int a3, int a4, int a5, uint8_t* state))(0x5D0C90);

		auto& objects = Blam::Objects::GetObjects();

		auto unitObjectIndex = ElDorito::GetMainTls(0xC4)[0](0x300 + 0xF8 * localPlayerIndex).Read<uint32_t>();
		if (unitObjectIndex != -1)
		{
			auto unitObjectPtr = Pointer(objects.Get(unitObjectIndex))[0xC];
			if (unitObjectPtr)
			{
				auto isDualWielding = unitObjectPtr(0x2CB).Read<uint8_t>() != 0xFF;
				auto isUsingController = *(bool*)0x0244DE98;

				if (!isUsingController && isDualWielding)
				{
					auto fireLeftAction = GetActionState(eGameActionFireLeft);
					auto fireRightAction = GetActionState(eGameActionFireRight);

					if (fireLeftAction->Ticks != 0 || fireRightAction->Ticks != 0)
					{
						ActionState tmp = *fireLeftAction;
						*fireLeftAction = *fireRightAction;
						*fireRightAction = tmp;
					}
				}
			}
		}

		LocalPlayerInputHook(localPlayerIndex, playerIndex, a3, a4, a5, state);
	}
}

namespace
{
	// These override the input type used to check each action, because the
	// defaults aren't very good and treat the UI actions as game input
	//
	// TODO: Use this with the controller input routine too!
	InputType actionInputTypes[eGameAction_KeyboardMouseCount] =
	{
		eInputTypeUi,      // eGameActionUiLeftTrigger
		eInputTypeUi,      // eGameActionUiRightTrigger
		eInputTypeUi,      // eGameActionUiUp
		eInputTypeUi,      // eGameActionUiDown
		eInputTypeUi,      // eGameActionUiLeft
		eInputTypeUi,      // eGameActionUiRight
		eInputTypeSpecial, // eGameActionUiStart
		eInputTypeSpecial, // eGameActionUiSelect
		eInputTypeUi,      // eGameActionUiLeftStick
		eInputTypeUi,      // eGameActionUiRightStick
		eInputTypeUi,      // eGameActionUiA
		eInputTypeUi,      // eGameActionUiB
		eInputTypeUi,      // eGameActionUiX
		eInputTypeUi,      // eGameActionUiY
		eInputTypeUi,      // eGameActionUiLeftBumper
		eInputTypeUi,      // eGameActionUiRightBumper
		eInputTypeGame,    // eGameActionJump
		eInputTypeGame,    // eGameActionSwitchGrenades
		eInputTypeGame,    // eGameActionSwitchWeapons
		eInputTypeGame,    // eGameActionUnk19
		eInputTypeGame,    // eGameActionReloadRight
		eInputTypeGame,    // eGameActionUse
		eInputTypeGame,    // eGameActionReloadLeft
		eInputTypeGame,    // eGameActionPickUpLeft
		eInputTypeGame,    // eGameActionMelee
		eInputTypeGame,    // eGameActionThrowGrenade
		eInputTypeGame,    // eGameActionFireRight
		eInputTypeGame,    // eGameActionFireLeft
		eInputTypeGame,    // eGameActionMeleeFire
		eInputTypeGame,    // eGameActionCrouch
		eInputTypeGame,    // eGameActionZoom
		eInputTypeGame,    // eGameActionUnk31
		eInputTypeGame,    // eGameActionUnk32
		eInputTypeGame,    // eGameActionSprint
		eInputTypeGame,    // eGameActionUnk34
		eInputTypeGame,    // eGameActionUnk35
		eInputTypeGame,    // eGameActionUnk36
		eInputTypeGame,    // eGameActionUnk37
		eInputTypeGame,    // eGameActionUnk38
		eInputTypeUi,      // eGameActionGeneralChat
		eInputTypeUi,      // eGameActionTeamChat
		eInputTypeGame,    // eGameActionUnk41
		eInputTypeGame,    // eGameActionUnk42
		eInputTypeGame,    // eGameActionUnk43
		eInputTypeGame,    // eGameActionUseConsumable1
		eInputTypeGame,    // eGameActionUseConsumable2
		eInputTypeGame,    // eGameActionUseConsumable3
		eInputTypeGame,    // eGameActionUseConsumable4
		eInputTypeGame,    // eGameActionVehicleBoost
		eInputTypeGame,    // eGameActionVehicleDive
		eInputTypeGame,    // eGameActionVehicleRaise
		eInputTypeGame,    // eGameActionVehicleAccelerate
		eInputTypeGame,    // eGameActionVehicleBrake
		eInputTypeGame,    // eGameActionVehicleFire
		eInputTypeGame,    // eGameActionVehicleAltFire
		eInputTypeGame,    // eGameActionVehicleExit
		eInputTypeUi,      // eGameActionUnk56
		eInputTypeUi,      // eGameActionUnk57
		eInputTypeUi,      // eGameActionUnk58
		eInputTypeGame,    // eGameActionMoveForward
		eInputTypeGame,    // eGameActionMoveBack
		eInputTypeGame,    // eGameActionMoveLeft
		eInputTypeGame,    // eGameActionMoveRight
	};
}
