on run
	tell application "Google Chrome"
		repeat with browserWindow in windows
			repeat with tabItem in tabs of browserWindow
				set tabURL to URL of tabItem
				if tabURL contains "ark.volcengine.com" and tabURL contains "/coding-plan" then
					return execute tabItem javascript "document.body.innerText"
				end if
			end repeat
		end repeat
	end tell
	return ""
end run
