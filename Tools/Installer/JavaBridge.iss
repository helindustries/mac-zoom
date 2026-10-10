// Included into the [Code] section of MacZoom.iss.
//
// The Java Access Bridge is switched on per user, in the .accessibility.properties every Java
// runtime reads at start-up -- the file jabswitch -enable writes. Setup edits it directly, so
// no Java runtime has to be found first, and records each change it made, so that uninstalling
// takes back exactly those and leaves whatever a screen reader or the user had set.

#ifndef JavaBridgeSettingsKey
  #define JavaBridgeSettingsKey "Software\MacZoom"
#endif

const
    JavaBridgeEntry = 'com.sun.java.accessibility.AccessBridge';
    TechnologiesKey = 'assistive_technologies=';
    MagnifierLine = 'screen_magnifier_present=true';
    SettingsKey = '{#JavaBridgeSettingsKey}';
    ChangesValue = 'SetupJavaBridgeChanges';
    CreatedFile = 1;
    AddedTechnologies = 2;
    AppendedEntry = 4;
    AddedMagnifier = 8;

function PropertiesPath(): String;
begin
    Result := ExpandConstant('{%USERPROFILE}\.accessibility.properties');
end;

procedure AddLine(var Lines: TArrayOfString; const Line: String);
begin
    SetArrayLength(Lines, GetArrayLength(Lines) + 1);
    Lines[GetArrayLength(Lines) - 1] := Line;
end;

procedure EnableJavaBridge();
var
    Lines: TArrayOfString;
    Changes, I: Integer;
    Loaded, HasTechnologies, HasMagnifier: Boolean;
begin
    Changes := 0;
    SetArrayLength(Lines, 0);
    if FileExists(PropertiesPath()) then
        Loaded := LoadStringsFromFile(PropertiesPath(), Lines)
    else
    begin
        Loaded := True;
        Changes := CreatedFile;
    end;

    if Loaded then
    begin
        HasTechnologies := False;
        HasMagnifier := False;
        for I := 0 to GetArrayLength(Lines) - 1 do
        begin
            if Pos(TechnologiesKey, Trim(Lines[I])) = 1 then
            begin
                HasTechnologies := True;
                if Pos(JavaBridgeEntry, Lines[I]) = 0 then
                begin
                    Lines[I] := Trim(Lines[I]) + ',' + JavaBridgeEntry;
                    Changes := Changes or AppendedEntry;
                end;
            end;
            if Trim(Lines[I]) = MagnifierLine then
                HasMagnifier := True;
        end;
        if not HasTechnologies then
        begin
            AddLine(Lines, TechnologiesKey + JavaBridgeEntry);
            Changes := Changes or AddedTechnologies;
        end;
        if not HasMagnifier then
        begin
            AddLine(Lines, MagnifierLine);
            Changes := Changes or AddedMagnifier;
        end;

        // Nothing to do where the bridge was on already, and nothing recorded either, so that
        // uninstalling leaves it on as found.
        if (Changes and not CreatedFile) <> 0 then
        begin
            if SaveStringsToFile(PropertiesPath(), Lines, False) then
                RegWriteDWordValue(HKCU, SettingsKey, ChangesValue, Changes)
            else
                MsgBox('Could not write ' + PropertiesPath() + ', so Java applications will not report their text cursor to {#AppName}.', mbError, MB_OK);
        end;
    end
    else
        MsgBox('Could not read ' + PropertiesPath() + ', so it was left alone and Java applications will not report their text cursor to {#AppName}.', mbError, MB_OK);
end;

procedure DisableJavaBridge();
var
    Lines, Kept: TArrayOfString;
    Changes: Cardinal;
    I: Integer;
    Line: String;
begin
    if RegQueryDWordValue(HKCU, SettingsKey, ChangesValue, Changes)
        and LoadStringsFromFile(PropertiesPath(), Lines) then
    begin
        SetArrayLength(Kept, 0);
        for I := 0 to GetArrayLength(Lines) - 1 do
        begin
            Line := Lines[I];
            if ((Changes and AppendedEntry) <> 0) and (Pos(TechnologiesKey, Trim(Line)) = 1) then
                StringChangeEx(Line, ',' + JavaBridgeEntry, '', True);
            if not ((((Changes and AddedTechnologies) <> 0) and (Trim(Line) = TechnologiesKey + JavaBridgeEntry))
                    or (((Changes and AddedMagnifier) <> 0) and (Trim(Line) = MagnifierLine))) then
                AddLine(Kept, Line);
        end;
        if ((Changes and CreatedFile) <> 0) and (GetArrayLength(Kept) = 0) then
            DeleteFile(PropertiesPath())
        else
            SaveStringsToFile(PropertiesPath(), Kept, False);
    end;
    RegDeleteValue(HKCU, SettingsKey, ChangesValue);
end;
