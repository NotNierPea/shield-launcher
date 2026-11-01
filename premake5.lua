dependencies = {
	basePath = "./deps"
}

function dependencies.load()
	dir = path.join(dependencies.basePath, "premake/*.lua")
	deps = os.matchfiles(dir)
	for i, dep in pairs(deps) do
		dep = dep:gsub(".lua", "")
		require(dep)
	end
end

function dependencies.imports()
	for i, proj in pairs(dependencies) do
		if type(i) == 'number' then
			proj.import()
		end
	end
end

function dependencies.projects()
    for i, proj in pairs(dependencies) do
        if type(i) == 'number' and proj.project then  -- Check if 'project' function exists
            proj.project()  -- Only call if the project function exists
        end
    end
end


newoption {
	trigger = "copy-to",
	description = "Optional, the BO4 game folder release builds also copy the exes into (or set SHIELD_BO4_DIR).",
	value = "PATH"
}

-- release builds copy their exe into the release files, and into the BO4 folder when one is set
local release_files_dir = path.getabsolute("project-bo4/Latest-Release-Files")
local bo4_dir = _OPTIONS["copy-to"] or os.getenv("SHIELD_BO4_DIR")

function copy_build_to(subdir)
	local function copy_cmds(root)
		local dest = path.translate(subdir ~= "" and path.join(root, subdir) or root, "\\")
		return {
			'if not exist "' .. dest .. '" mkdir "' .. dest .. '"',
			'copy /B /Y "$(TargetPath)" "' .. dest .. '\\$(TargetFileName)" > nul',
		}
	end

	filter "configurations:Release"
		postbuildcommands(copy_cmds(release_files_dir))
		if bo4_dir then
			postbuildcommands(copy_cmds(bo4_dir))
		end
	filter {}
end

newoption {
	trigger = "dev-build",
	description = "Enable development builds of the client."
}

dependencies.load()

-- Define the main project 

workspace "Shield_launcher"

	startproject "launcher"
	location "./build"
	objdir "%{wks.location}/obj"
	targetdir "%{wks.location}/bin/%{cfg.platform}/%{cfg.buildcfg}"

	configurations {"Debug", "Release"}

	language "C++"
	cppdialect "C++20"

	architecture "x86_64"
	platforms "x64"
	staticruntime "off"
    runtime "Release"

	systemversion "latest"
	symbols "On"
	editandcontinue "Off"
	warnings "Extra"
	characterset "ASCII"

	if _OPTIONS["dev-build"] then
		defines {"DEV_BUILD"}
	end

	if os.getenv("CI") then
		defines {"CI"}
	end

    incrementallink "Off"
	minimalrebuild "Off"
	multiprocessorcompile "On"
	enable64bitchecks "Off"

	filter "platforms:x64"
		defines {"_WINDOWS", "WIN32"}
	filter {}

	filter "configurations:Release"
		optimize "Size"
		buildoptions {"/GL", "/Zc:__cplusplus"}
		linkoptions { "/IGNORE:4702", "/LTCG" }
		defines {"NDEBUG"}
	filter {}

	filter "configurations:Debug"
		--optimize "Debug"
		--defines {"DEBUG", "_DEBUG"}
                runtime "Debug"
		defines { "_DEBUG" }
                buildoptions { "/Zc:__cplusplus" }
   	        symbols "On"
	filter {}

		
-- Define the utlis project
project "utilities"
	kind "StaticLib"
	language "C++"

	files {"./source/utilities/**.hpp", "./source/utilities/**.cpp"}

	includedirs {"./source/utilities", "%{prj.location}/source"}

	dependencies.imports()
	
	
-- the launcher, a WebView2 window whose page is built into the exe from source/launcher/ui
project "launcher"
    kind "WindowedApp"
    language "C++"
    targetname "Shield_Launcher"
    cppdialect "C++20"

    pchheader "std_include.hpp"
    pchsource "source/launcher/std_include.cpp"
    
    includedirs
    {
        "source/launcher",
        "source/utilities", 
        "%{prj.location}/source"
    }

    files 
    {
        "./source/launcher/resources.rc",
        "./source/launcher/ui.rc",
        "./source/launcher/**.hpp",
        "./source/launcher/**.cpp",
        "./source/launcher/**.h",
        "./source/launcher/**.ico",
        "./source/launcher/**.manifest",
        "./source/launcher/ui/**"
    }

    links {
        "utilities",  -- Links with utilities
    }

    -- VS restores the SDK on build, the static loader keeps WebView2Loader.dll out of the release
    local webview2_version = "1.0.4191.47"
    nuget { "Microsoft.Web.WebView2:" .. webview2_version }

    -- premake imports build\Common.targets directly, so set what the package's native wrapper would
    vsprops {
        WebView2ProjectKind = "native",
        NugetRoot = "$(MSBuildProjectDirectory)\\packages\\Microsoft.Web.WebView2." .. webview2_version .. "\\",
        WebView2LoaderPreference = "Static"
    }

    -- ui edits only touch ui.rc's inputs, so let MSBuild decide instead of the fast check
    fastuptodate "Off"

    -- building the launcher also builds (and copies) the shortcut
    dependson { "launcher_shortcut" }

    copy_build_to("project-bo4/launcher")

    dependencies.imports()

group "Dependencies"
    dependencies.projects()

-- Define the launcher shortcut project
project "launcher_shortcut"
    kind "WindowedApp"
    language "C++"
    targetname "Launch Project BO4"
    cppdialect "C++20"
    
    files 
    {
        "./source/launcher_shortcut/**.cpp",
        "./source/launcher_shortcut/**.rc",
        "./source/launcher_shortcut/**.ico"
    }
    
    includedirs
    {
        "source/launcher_shortcut"
    }
    
    -- VS skips up to date projects and their post-build, so the copy would never run
    fastuptodate "Off"

    copy_build_to("")

    filter "configurations:Release"
        optimize "Size"
        defines {"NDEBUG"}
    filter {}
    
    filter "configurations:Debug"
        defines {"_DEBUG"}
        symbols "On"
    filter {}
