# ak-dualsense

full dualsense support for batman arkham knight on pc (steam), over **bluetooth**. no cable needed.

the game only ever supported the original ds4 (wired, static red light bar, normal rumble). this replaces the game's `libScePad_x64.dll` with one that talks to the dualsense directly and adds a [BmSDK](https://github.com/Team-BmSDK/BmSDK-AK) script that reads what's going on in the game and drives the controller from it.

## notice

1. its vibecoded. at first i built it for myself to have a better experience with a first playthrough of the game, but i eventually decided to publish it. if someone doesn't like the fact that it's vibecoded they shouldn't use it
2. contributions are strongly advised as the hands-on testing i've done is minimal and i'm still at the beginning of the game, plus the project is open source. all i ask is for some credit if used elsewhere.

## what it does

**adaptive triggers**
- batmobile: R2 gets heavier the faster you go, goes loose when you're in the air, vibrates while boosting. L2 is brake / reverse (like every other driving game) with a pedal feel and an abs shudder when you brake hard
- battle mode: 60mm cannon has a gun click, the vulcan gun rattles, missiles feel like pulling a bow, emp buzzes, R2 locks up when the vulcan overheats
- on foot: L2 has a light pull when you aim, R2 changes per gadget (batarang, batclaw, line launcher, rec, gel, disruptor, remote hacking device...)

**haptics over bluetooth**
- the game's rumble gets turned into proper haptics instead of the controller's fake rumble
- extra effects for cannon shots, missiles, emp, boost, dodges, grapple boost, gadget throws and getting hit (it hits on the left or right side depending on where it came from)
- engine feel in the batmobile that follows the rpm

**light bar**
- character colour (batman, robin, nightwing, catwoman, azrael, red hood, harley, batgirl)
- darker in predator areas, red tint in fights, amber at low health, pulsing red when you're about to die, white flash when you get hit
- detective mode, gliding, each gadget while aiming
- batmobile: blue that gets brighter with speed, boost, battle mode, cannon charge, vulcan overheat, low car health
- dims when paused or in cutscenes

**everything else**
- player leds under the touchpad are a meter: health on foot, boost in the batmobile, vulcan heat in battle mode. they show the battery level for a few seconds when the controller connects
- gyro aiming while holding L2 and in battle mode. the mute button turns gyro on/off (orange light = off)
- touchpad click works like in the game, swipes act as the d-pad
- light bar flashes red if the battery is almost dead
- the speaker is never used

## requirements

- batman arkham knight on steam
- [BmSDK](https://github.com/Team-BmSDK/BmSDK-AK/releases) installed (tested with 0.21.0)
- a dualsense (or dualsense edge) paired with windows over bluetooth. usb works too but haptics are only over bluetooth
- windows 10 2004 or newer

## install

1. install BmSDK if you don't have it
2. download the zip from [releases](../../releases), extract it anywhere
3. run `install.bat`. it finds the game on its own, if it doesn't then run it with the game folder: `install.bat "D:\SteamLibrary\steamapps\common\Batman Arkham Knight"`
4. in steam: arkham knight > properties > controller > **disable steam input**
5. play

the first time you get in the batmobile the script switches battle mode to the R1 toggle (it's the game's own option), so L2 can be the brake. you can change it back in the controls menu if you want L2 for battle mode.

if the button prompts show xbox buttons get a ps icons mod, [Dualshock 4 Controller Icons](https://www.nexusmods.com/batmanarkhamknight/mods/3001) works fine with this. the installer changes the batmobile brake prompt to L2.

`uninstall.bat` puts the original files back.

## settings

`Binaries\Win64\ak_dualsense.ini` (made on first run if it's missing)

| setting | what |
| --- | --- |
| `intensity` | `soft`, `normal` or `strong`. the quick way to make everything lighter or heavier, normal is the default |
| `rumble` | `hd` = haptics over bluetooth, `classic` = the controller's own rumble emulation |
| `rumble_strength` / `haptics_volume` | 0-200, on top of `intensity` |
| `triggers` / `trigger_strength` | 1/0, 0-150 on top of `intensity` |
| `lightbar_brightness` | 0-100 |
| `player_leds` | 1/0 |
| `gyro` | `off`, `l2` (while aiming), `always` |
| `gyro_sens_x` / `gyro_sens_y` | sensitivity |
| `gyro_invert_x` / `gyro_invert_y` | 1 to invert |
| `gyro_anti_deadzone` | % pushed past the game's stick deadzone |
| `mute_toggles_gyro` | 1/0 |
| `touch_swipes` | 1/0 |
| `battery_on_connect` / `low_battery_warning` | 1/0 |
| `verbose_log` | logs more stuff to `ak_dualsense.log` |

## if something's wrong

check `Binaries\Win64\ak_dualsense.log` first. it says if it found the controller and if the script attached.

there's also `tools\check.exe` in the zip, run it with the game closed and it tests the light bar, rumble, haptics, triggers and gyro one by one.

## building

needs visual studio 2022 build tools (c++ workload). run `build.bat`, the dll ends up in `build\`.

## credits

- [SAxense](https://github.com/egormanga/SAxense) by egormanga for figuring out the bluetooth haptics report, without it there's no haptics over bluetooth
- [BmSDK](https://github.com/Team-BmSDK/BmSDK-AK) for making scripting this game possible
- linux's hid-playstation driver and [dualsensectl](https://github.com/nowrep/dualsensectl) for documenting the dualsense reports and trigger effects

## license

MIT
