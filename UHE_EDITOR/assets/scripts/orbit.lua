-- An entity that orbits the origin, and says where it is.
--
-- A script is attached with a ScriptComponent; the engine runs OnCreate once,
-- OnUpdate every step with the frame's dt, and OnDestroy when the entity goes
-- away. `self` is the entity: it has the transform methods, the rigid-body
-- methods, and any field the script puts on it.

local angle = 0.0
local ticks = 0

function OnCreate(self)
    self.radius = 2.0
    self.height = 0.5
    Log.info("orbit: " .. self:GetName() .. " created")
end

function OnUpdate(self, dt)
    angle = angle + dt
    ticks = ticks + 1
    self:SetPosition(math.cos(angle) * self.radius, self.height, math.sin(angle) * self.radius)

    if ticks % 120 == 0 then
        local p = self:GetPosition()
        Log.info(string.format("orbit: step %d at (%.2f, %.2f)", ticks, p.x, p.z))
    end
end

function OnDestroy(self)
    Log.info("orbit: " .. self:GetName() .. " destroyed")
end
