-- driven by tests/hlua_test.cpp, prints HLUATEST markers it checks back

local passed, failed = 0, 0

local function ok( name, cond )
        if cond then
                passed = passed + 1
                print( "HLUATEST OK " .. name )
        else
                failed = failed + 1
                print( "HLUATEST FAIL " .. name )
        end
end

ok( "server_flag", SERVER == true and CLIENT == false )
ok( "globals", type( hook ) == "table" and type( timer ) == "table"
        and type( ents ) == "table" and type( CurTime ) == "function" )

ents.Register( "npc_drone", {
        OnSpawn = function( self )
                ok( "drone_spawn", self:GetClass() == "npc_drone" )
                self:SetModel( "models/battery.mdl" )
                self:GetTable().fuel = 42
                self:NextThink( 0.5 )
        end,
        OnThink = function( self )
                ok( "drone_think", true )
                ok( "drone_table", self:GetTable().fuel == 42 )
                self:Remove()
        end,
        OnRemove = function( self )
                ok( "drone_remove", true )
        end,
} )

ents.Register( "func_breakable", {
        OnSpawn = function( self )
                ok( "override_spawn", self:GetClass() == "func_breakable" )
                local kv = self:GetTable().kv
                ok( "override_kv", kv and kv.reward == "50" )
        end,
        OnThink = function( self )
                ok( "rebound_think", self:GetClass() == "func_breakable" )
        end,
} )

hook.Add( "InitPostEntity", "test", function()
        ok( "init_post_entity", true )

        local e = ents.Create( "npc_drone" )
        ok( "create", IsValid( e ) )
        ok( "create_class", e and e:GetClass() == "npc_drone" )
        e:SetPos( Vector( 1, 2, 3 ) )
        ok( "create_pos", e:GetPos().x == 1 and e:GetPos().z == 3 )

        ok( "create_unknown", ents.Create( "no_such_class" ) == nil )
        ok( "findbyclass", #ents.FindByClass( "npc_drone" ) >= 1 )
        ok( "override_found", #ents.FindByClass( "func_breakable" ) == 1 )
        ok( "find_players", #ents.FindByClass( "player" ) == 1 )
end )

hook.Add( "PlayerConnect", "test", function( name, address )
        ok( "connect_hook", name == "tester" and address == "127.0.0.1" )
        return "no testers allowed"
end )

hook.Add( "PlayerCommand", "test", function( pl, args )
        if args[1] == "makemecoffee" then
                ok( "command_hook", pl ~= nil )
                return true
        end
end )

hook.Add( "PlayerSay", "test", function( pl, text, teamonly )
        if text == "hello world" then
                ok( "say_hook", teamonly == false )
                return "filtered"
        end
        if text == "blockme" then
                return false
        end
end )

hook.Add( "EntityRemoved", "test", function( ent )
        if ent:GetClass() == "npc_drone" then
                ok( "entity_removed", true )
        end
end )

hook.Add( "StartFrame", "test", function()
        if CurTime() >= 2.0 then
                print( string.format( "HLUATEST SUMMARY %d %d", passed, failed ) )
                hook.Remove( "StartFrame", "test" )
        end
end )

timer.Simple( 1.5, function()
        ok( "drone_gone", #ents.FindByClass( "npc_drone" ) == 0 )
end )

timer.Simple( 1.0, function()
        ok( "timer_simple", true )
end )

timer.Create( "beat", 0.4, 0, function()
        ok( "timer_repeat", true )
        timer.Remove( "beat" )
end )
