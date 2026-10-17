using System.Numerics;
using System.Runtime.InteropServices;
using BmSDK;
using BmSDK.BmGame;
using BmSDK.BmScript;
using BmSDK.Engine;

[Script]
public class DualSenseScript : Script
{
    const string Pad = "libScePad_x64.dll";

    [DllImport(Pad)] static extern void ds_Heartbeat();
    [DllImport(Pad)] static extern void ds_TriggerOff(int side);
    [DllImport(Pad)] static extern void ds_TriggerFeedback(int side, int pos, int strength);
    [DllImport(Pad)] static extern void ds_TriggerSlope(int side, int start, int end, int from, int to);
    [DllImport(Pad)] static extern void ds_TriggerWeapon(int side, int start, int end, int strength);
    [DllImport(Pad)] static extern void ds_TriggerVibration(int side, int pos, int amp, int freq);
    [DllImport(Pad)] static extern void ds_TriggerBow(int side, int start, int end, int strength, int snap);
    [DllImport(Pad)] static extern void ds_TriggerMachine(int side, int start, int end, int a, int b, int freq, int period);
    [DllImport(Pad)] static extern void ds_TriggerGalloping(int side, int start, int end, int foot1, int foot2, int freq);
    [DllImport(Pad)] static extern void ds_Haptic(int kind, float strength);
    [DllImport(Pad)] static extern void ds_Engine(float rpm, float load);
    [DllImport(Pad)] static extern void ds_GyroAim(int on);
    [DllImport(Pad)] static extern void ds_BrakeOnL2(int on);
    [DllImport(Pad)] static extern void ds_Lightbar(int r, int g, int b);
    [DllImport(Pad)] static extern void ds_LightbarRelease();
    [DllImport(Pad)] static extern void ds_PlayerLedBar(float fill);
    [DllImport(Pad)] static extern void ds_PlayerLeds(int mask);
    [DllImport(Pad)] static extern void ds_AudioHaptics(float level);
    [DllImport(Pad)] static extern void ds_Log([MarshalAs(UnmanagedType.LPStr)] string text);

    const int Left = 0;
    const int Right = 1;

    static bool s_firing;
    static double s_firingSince;
    static bool s_broken;
    static double s_hitUntil;
    static bool s_toggleChecked;
    static readonly System.Diagnostics.Stopwatch s_clock = System.Diagnostics.Stopwatch.StartNew();

    string _leftNow = "";
    string _rightNow = "";
    string _lastMode = "";
    double _nextShot;
    double _lastTick;
    int _lr = -1, _lg = -1, _lb = -1;
    int _errors;

    public override void Main() => Startup();

    public override void OnLoad() => Startup();

    public override void OnUnload()
    {
        try
        {
            ds_TriggerOff(2);
            ds_Engine(0, 0);
            ds_GyroAim(0);
        }
        catch
        {
        }
    }

    void Startup()
    {
        try
        {
            ds_Log("dualsense script loaded");
            s_broken = false;
        }
        catch (Exception e)
        {
            s_broken = true;
            Debug.LogWarning("DualSense: pad dll not reachable, " + e.Message);
        }
    }

    public override void OnTick()
    {
        if (s_broken)
            return;
        try
        {
            Update();
        }
        catch (Exception e)
        {
            _errors++;
            if (_errors <= 3)
                Debug.LogWarning("DualSense: " + e.Message);
            Release();
        }
    }

    void SetLeft(string key, Action set)
    {
        if (_leftNow == key)
            return;
        _leftNow = key;
        set();
    }

    void SetRight(string key, Action set)
    {
        if (_rightNow == key)
            return;
        _rightNow = key;
        set();
    }

    void Release()
    {
        SetLeft("off", () => ds_TriggerOff(Left));
        SetRight("off", () => ds_TriggerOff(Right));
        ds_Engine(0, 0);
        ds_GyroAim(0);
        ds_BrakeOnL2(0);
        if (_lr != -1)
        {
            ds_LightbarRelease();
            _lr = _lg = _lb = -1;
        }
    }

    void Light(float r, float g, float b)
    {
        double now = s_clock.Elapsed.TotalSeconds;
        if (now < s_hitUntil)
        {
            float k = (float)((s_hitUntil - now) / 0.15);
            r += (255 - r) * k;
            g += (255 - g) * k;
            b += (255 - b) * k;
        }
        int ir = (int)Math.Clamp(r, 0, 255), ig = (int)Math.Clamp(g, 0, 255), ib = (int)Math.Clamp(b, 0, 255);
        if (Math.Abs(ir - _lr) < 5 && Math.Abs(ig - _lg) < 5 && Math.Abs(ib - _lb) < 5)
            return;
        _lr = ir;
        _lg = ig;
        _lb = ib;
        ds_Lightbar(ir, ig, ib);
    }

    RPawnPlayerCombat _who;
    float[] _base = { 0, 40, 170 };

    float[] BaseColor(RPlayerController pc)
    {
        var p = pc.CombatPawn;
        if (p == null || p == _who)
            return _base;
        _who = p;
        if (p.IsRobin())
            _base = new float[] { 255, 20, 0 };
        else if (p.IsNightwing())
            _base = new float[] { 0, 90, 255 };
        else if (p.IsCatwoman())
            _base = new float[] { 140, 0, 255 };
        else if (p.IsAzrael())
            _base = new float[] { 255, 255, 230 };
        else if (p.IsRedHood())
            _base = new float[] { 200, 0, 0 };
        else if (p.IsHarleyQuinn())
            _base = new float[] { 255, 0, 120 };
        else if (p.IsBatGirl())
            _base = new float[] { 160, 0, 220 };
        else
            _base = new float[] { 0, 40, 170 };
        return _base;
    }

    void FootLight(RPlayerController pc, RInventoryGadget gadget, bool aiming)
    {
        var me = pc.CombatPawn;
        float health = me != null && me.HealthMax > 0 ? (float)me.Health / me.HealthMax : 1;
        ds_PlayerLedBar(Clamp01(health));
        var c = BaseColor(pc);
        var gri = Game.GetWorldInfo().GRI as RGameRI;
        bool combat = gri != null && gri.bBatmanInCombat;
        bool predator = pc.PlayerPredatorZone != null;
        float t = (float)s_clock.Elapsed.TotalSeconds;

        if (health < 0.25f)
        {
            Light(255 * Pulse(4), 0, 0);
            return;
        }
        if (pc.bInvestigateMode)
        {
            float shimmer = 0.85f + 0.15f * (float)Math.Sin(t * 2.5);
            Light(0, 150 * shimmer, 255 * shimmer);
            return;
        }
        if (aiming && gadget != null)
        {
            if (gadget is RMagneticBlast)
                Light(60 * Flicker(), 140 * Flicker(), 255);
            else if (gadget is RGooSpray)
                Light(255, 90, 0);
            else if (gadget is RDisruptorSniper)
                Light(0, 255, 60);
            else if (gadget is RRHD)
                Light(0, 255 * Pulse(6), 170);
            else if (gadget is RFreezeClusterGrenade || gadget is RFreezeSpray)
                Light(150, 230, 255);
            else if (gadget is RVoiceSynthesiser)
                Light(150, 0, 255);
            else if (gadget is RHarpoonGun || gadget is RLineLauncher || gadget is RGrappleGun)
                Light(255, 170, 60);
            else
                Light(170, 200, 255);
            return;
        }
        if (pc.CapeGlideSpeed > 0 || pc.BoostGlide)
        {
            float k = Clamp01(pc.CapeGlideSpeed / 2000);
            Light(c[0] + (200 - c[0]) * k, c[1] + (220 - c[1]) * k, c[2] + (255 - c[2]) * k);
            return;
        }
        if (health < 0.5f)
        {
            float k = (health - 0.25f) / 0.25f;
            Light(220 + (c[0] - 220) * k, 70 + (c[1] - 70) * k, c[2] * k);
            return;
        }
        if (combat)
        {
            Light(Math.Max(c[0], 90), c[1] * 0.6f, c[2] * 0.8f);
            return;
        }
        if (predator)
        {
            Light(c[0] * 0.25f, c[1] * 0.25f, c[2] * 0.35f);
            return;
        }
        Light(c[0], c[1], c[2]);
    }

    static readonly Random s_rng = new();

    float Flicker() => 0.6f + 0.4f * (float)s_rng.NextDouble();

    float Pulse(float speed) => (float)(0.35 + 0.65 * Math.Abs(Math.Sin(s_clock.Elapsed.TotalSeconds * speed)));

    static float Clamp01(float v) => v < 0 ? 0 : v > 1 ? 1 : v;

    void Update()
    {
        double now = s_clock.Elapsed.TotalSeconds;
        if (now - _lastTick > 0.25)
        {
            _leftNow = "";
            _rightNow = "";
            _lr = _lg = _lb = -1;
        }
        _lastTick = now;
        ds_Heartbeat();
        RPlayerController pc;
        try
        {
            pc = Game.GetPlayerController();
        }
        catch
        {
            Release();
            return;
        }
        if (pc == null || !pc.IsValid)
        {
            Release();
            return;
        }

        var wi = Game.GetWorldInfo();
        if (wi == null || wi.Pauser != null || pc.bCinematicMode || pc.IsLoadingScreenVisible())
        {
            SetMode("idle");
            SetLeft("off", () => ds_TriggerOff(Left));
            SetRight("off", () => ds_TriggerOff(Right));
            ds_Engine(0, 0);
            ds_GyroAim(0);
            ds_BrakeOnL2(0);
            var c = BaseColor(pc);
            Light(c[0] * 0.3f, c[1] * 0.3f, c[2] * 0.3f);
            ds_PlayerLeds(4);
            return;
        }

        var bm = pc.BatmobilePawn;
        if (pc.Pawn is RVehicleBatmobileBase && bm != null && bm.IsValid)
            Driving(pc, bm);
        else
            OnFoot(pc);
    }

    void SetMode(string mode)
    {
        if (mode == _lastMode)
            return;
        _lastMode = mode;
        ds_Log("mode " + mode);
    }

    void Driving(RPlayerController pc, RVehicleBatmobileBase bm)
    {
        if (!s_toggleChecked)
        {
            s_toggleChecked = true;
            if (pc.PlayerInput is RPlayerInput input && !input.bBatmobileBattleModeToggle)
            {
                pc.FE_SetBattleModeToggle(true);
                ds_Log("battle mode moved to the R1 toggle so L2 can brake");
            }
        }
        bool battle = bm.IsInBattleMode();
        // float max = bm.MaxSpeed > 1 ? bm.MaxSpeed : 3000;
        float max = 2400;
        float speed = Clamp01(Math.Abs(bm.ForwardVel) / max);
        float rpm = Clamp01((bm.EngineRPM - 500) / 4500);
        float hp = bm.HealthMax > 0 ? (float)bm.Health / bm.HealthMax : 1;
        float gas = Clamp01(Math.Abs(bm.Throttle));
        bool boost = bm is RVehicleBatmobile car && car.bAfterburnerIsOn;

        ds_AudioHaptics(0.7f);

        if (!battle)
        {
            SetMode(bm.bRemoteDrive ? "batmobile remote" : "batmobile pursuit");
            ds_GyroAim(0);
            ds_BrakeOnL2(1);
            ds_PlayerLedBar(Clamp01(bm.GetBoostPct()));
            ds_Engine(boost ? 1 : 0.1f + rpm * 0.9f, boost ? 1 : gas * (0.35f + rpm * 0.65f));
            if (hp < 0.3f)
                Light(255 * Pulse(5), 0, 0);
            else if (boost)
                Light(150, 210, 255);
            else if (bm.bRemoteDrive)
                Light(0, 200, 120);
            else
                Light(0, 50 + speed * 40, 255);
            if (boost)
                SetRight("boost", () => ds_TriggerVibration(Right, 1, 7, 45));
            else if (bm.AirborneTime > 0.25f)
                SetRight("air", () => ds_TriggerFeedback(Right, 0, 1));
            else
            {
                int level = 2 + (int)(speed * 4);
                SetRight("drive" + level, () => ds_TriggerSlope(Right, 1, 8, 1, level));
            }
            float brake = Clamp01(-bm.Throttle);
            if (brake > 0.5f && bm.ForwardVel > max * 0.3f)
                SetLeft("abs", () => ds_TriggerVibration(Left, 2, 3, 28));
            else
                SetLeft("brake", () => ds_TriggerSlope(Left, 1, 8, 1, 4));
            return;
        }

        SetMode("batmobile battle");
        ds_GyroAim(1);
        ds_BrakeOnL2(0);
        ds_Engine(0.05f + rpm * 0.5f, 0.1f + speed * 0.3f);
        SetLeft("off", () => ds_TriggerOff(Left));

        var wpn = bm.CurrentWeapon;
        if (wpn is RBatmobileCannon heat)
            ds_PlayerLedBar(Clamp01(heat.CurrentHeatLevel > 1 ? heat.CurrentHeatLevel / 100 : heat.CurrentHeatLevel));
        else
            ds_PlayerLedBar(hp);
        if (hp < 0.3f)
            Light(255 * Pulse(5), 0, 0);
        else if (wpn is RBatmobileCannon hot && hot.bOverheating)
            Light(255, 120 * Flicker(), 0);
        else if (wpn is RBatmobileHeavyCannon big && big.ChargingShot)
            Light(255, 35 + 200 * Clamp01(big.ChargeProp), 200 * Clamp01(big.ChargeProp));
        else if (wpn is RBatmobileEMP emp && emp.ChargingEMP)
            Light(80 * Flicker(), 160, 255);
        else
            Light(255, 35, 0);
        if (wpn is RBatmobileCannon gun)
        {
            if (gun.bOverheating)
                SetRight("overheat", () => ds_TriggerFeedback(Right, 1, 8));
            else
                SetRight("vulcan", () => ds_TriggerMachine(Right, 1, 9, 6, 4, 18, 2));
            if (s_firing && !gun.bOverheating && s_clock.Elapsed.TotalSeconds >= _nextShot)
            {
                _nextShot = s_clock.Elapsed.TotalSeconds + 0.075;
                ds_Haptic(5, 0.55f);
            }
        }
        else if (wpn is RBatmobileHeavyCannon)
            SetRight("cannon", () => ds_TriggerWeapon(Right, 3, 6, 8));
        else if (wpn is RBatmobileMissileLauncher)
            SetRight("missiles", () => ds_TriggerBow(Right, 1, 7, 6, 8));
        else if (wpn is RBatmobileEMP)
            SetRight("emp", () => ds_TriggerVibration(Right, 2, 5, 70));
        else
            SetRight("battle", () => ds_TriggerWeapon(Right, 3, 6, 6));
    }

    void OnFoot(RPlayerController pc)
    {
        ds_Engine(0, 0);
        ds_BrakeOnL2(0);
        ds_AudioHaptics(0);
        RInventoryGadget gadget = null;
        try
        {
            gadget = pc.GetCurrentGadget();
        }
        catch
        {
        }
        bool aiming = pc.bAimGadgetPressed || (gadget != null && gadget.bAiming);
        FootLight(pc, gadget, aiming);
        if (!aiming)
        {
            SetMode("on foot");
            ds_GyroAim(0);
            SetLeft("off", () => ds_TriggerOff(Left));
            SetRight("off", () => ds_TriggerOff(Right));
            return;
        }

        string name = gadget == null ? "none" : gadget.Class.Name;
        SetMode("aiming " + name);
        ds_GyroAim(1);
        SetLeft("aim", () => ds_TriggerFeedback(Left, 1, 2));

        if (gadget is RBatarang)
            SetRight("batarang", () => ds_TriggerWeapon(Right, 2, 5, 4));
        else if (gadget is RHarpoonGun)
            SetRight("batclaw", () => ds_TriggerBow(Right, 1, 6, 5, 7));
        else if (gadget is RLineLauncher)
            SetRight("line", () => ds_TriggerSlope(Right, 1, 6, 1, 5));
        else if (gadget is RMagneticBlast)
            SetRight("rec", () => ds_TriggerWeapon(Right, 4, 7, 7));
        else if (gadget is RGooSpray)
            SetRight("gel", () => ds_TriggerGalloping(Right, 1, 8, 2, 5, 6));
        else if (gadget is RDisruptorSniper)
            SetRight("disruptor", () => ds_TriggerWeapon(Right, 5, 7, 8));
        else if (gadget is RRHD)
            SetRight("rhd", () => ds_TriggerVibration(Right, 2, 3, 90));
        else if (gadget is RGrappleGun)
            SetRight("grapple", () => ds_TriggerWeapon(Right, 2, 4, 3));
        else
            SetRight("gadget", () => ds_TriggerWeapon(Right, 3, 6, 5));
    }

    static void Safe(Action a)
    {
        try
        {
            a();
        }
        catch
        {
        }
    }

    [Redirect(typeof(RBatmobileHeavyCannon), nameof(RBatmobileHeavyCannon.FireWeapon))]
    static bool HeavyCannonFired(RBatmobileHeavyCannon self)
    {
        var fired = self.FireWeapon();
        if (fired)
            Safe(() => ds_Haptic(2, 0.8f));
        return fired;
    }

    [Redirect(typeof(RBatmobileMissileLauncher), nameof(RBatmobileMissileLauncher.FireMissilesNow))]
    static void MissilesFired(RBatmobileMissileLauncher self)
    {
        self.FireMissilesNow();
        Safe(() => ds_Haptic(11, 0.7f));
    }

    [Redirect(typeof(RBatmobileEMP), nameof(RBatmobileEMP.FireWeapon))]
    static bool EmpFired(RBatmobileEMP self)
    {
        var fired = self.FireWeapon();
        if (fired)
            Safe(() => ds_Haptic(4, 0.9f));
        return fired;
    }

    [Redirect(typeof(RBatmobileCannon), nameof(RBatmobileCannon.StartFiringLoop))]
    static void GunStarted(RBatmobileCannon self)
    {
        self.StartFiringLoop();
        s_firing = true;
        s_firingSince = s_clock.Elapsed.TotalSeconds;
    }

    [Redirect(typeof(RBatmobileCannon), nameof(RBatmobileCannon.StopFiringLoop))]
    static void GunStopped(RBatmobileCannon self)
    {
        self.StopFiringLoop();
        s_firing = false;
    }

    [Redirect(typeof(RVehicleBatmobile), nameof(RVehicleBatmobile.AfterburnerOn))]
    static void BoostOn(RVehicleBatmobile self, bool front, bool sideBurst)
    {
        self.AfterburnerOn(front, sideBurst);
        Safe(() => ds_Haptic(2, 0.6f));
    }

    [Redirect(typeof(RVehicleBatmobileBase), nameof(RVehicleBatmobileBase.BattleDodge))]
    static void Dodged(RVehicleBatmobileBase self, Vector3 direction)
    {
        self.BattleDodge(direction);
        Safe(() => ds_Haptic(1, 0.7f));
    }

    [Redirect(typeof(RVehicleBatmobileBase), nameof(RVehicleBatmobileBase.TakeDamage))]
    static void CarHit(RVehicleBatmobileBase self, int damage, Controller instigatedBy, Vector3 hitLocation, Vector3 momentum,
        Class damageType, Actor.FTraceHitInfo hitInfo, Actor damageCauser)
    {
        self.TakeDamage(damage, instigatedBy, hitLocation, momentum, damageType, hitInfo, damageCauser);
        if (damage > 0)
        {
            s_hitUntil = s_clock.Elapsed.TotalSeconds + 0.15;
            Safe(() => ds_Haptic(HitSide(self, hitLocation), Math.Min(1.0f, 0.4f + damage / 60.0f)));
        }
    }

    [Redirect(typeof(RPlayerControllerCombat), nameof(RPlayerControllerCombat.NotifyTakeDamage))]
    static void PlayerHit(RPlayerControllerCombat self, int damage, Controller instigatedBy, Vector3 hitLocation, Vector3 momentum,
        Class damageType, Actor.FTraceHitInfo hitInfo, Actor damageCauser)
    {
        self.NotifyTakeDamage(damage, instigatedBy, hitLocation, momentum, damageType, hitInfo, damageCauser);
        if (damage > 0)
            s_hitUntil = s_clock.Elapsed.TotalSeconds + 0.15;
        if (damage > 0)
            Safe(() => ds_Haptic(self.CombatPawn != null ? HitSide(self.CombatPawn, hitLocation) : 1, Math.Min(1.0f, 0.4f + damage / 40.0f)));
    }

    static int HitSide(Actor victim, Vector3 hitLocation)
    {
        try
        {
            var rot = victim.Rotation;
            double yaw = rot.Yaw * Math.PI / 32768.0;
            var right = new Vector3((float)-Math.Sin(yaw), (float)Math.Cos(yaw), 0);
            var dir = hitLocation - victim.Location;
            float side = Vector3.Dot(dir, right);
            if (side > 30)
                return 7;
            if (side < -30)
                return 6;
        }
        catch
        {
        }
        return 1;
    }

    [Redirect(typeof(RInventoryGadget), nameof(RInventoryGadget.GadgetUsed))]
    static void GadgetUsed(RInventoryGadget self)
    {
        self.GadgetUsed();
        Safe(() => ds_Haptic(0, 0.8f));
    }

    [Redirect(typeof(RInventoryGadget), nameof(RInventoryGadget.ThrowGadget))]
    static void GadgetThrown(RInventoryGadget self, Vector3 from, RBMAIController.BRAECReactionType type, int launchId)
    {
        self.ThrowGadget(from, type, launchId);
        Safe(() => ds_Haptic(10, 0.8f));
    }

    [Redirect(typeof(RPlayerController), nameof(RPlayerController.StartGrappleBoost))]
    static void GrappleBoost(RPlayerController self)
    {
        self.StartGrappleBoost();
        Safe(() => ds_Haptic(8, 0.8f));
    }
}
