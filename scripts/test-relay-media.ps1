param([string]$Origin='http://127.0.0.1:8787')
$ErrorActionPreference='Stop'
$relayUri=[Uri]$Origin
if($relayUri.Scheme -ne 'http' -or $relayUri.Host -ne '127.0.0.1' -or $relayUri.AbsolutePath -ne '/'){
    throw 'This component check is restricted to a local relay emulator.'
}
$connections=[Collections.Generic.List[Net.WebSockets.ClientWebSocket]]::new()
$cancel=[Threading.CancellationTokenSource]::new([TimeSpan]::FromSeconds(30))
$headers=@{'X-Beetleworx-Version'='0.1.0-demo.4';'CF-Connecting-IP'='127.0.0.1'}
function NewRelaySocket([string]$Path,[string]$Authorization=''){
    $socket=[Net.WebSockets.ClientWebSocket]::new()
    $connections.Add($socket)
    $socket.Options.CollectHttpResponseDetails=$true
    foreach($entry in $headers.GetEnumerator()){$socket.Options.SetRequestHeader($entry.Key,$entry.Value)}
    if($Authorization){$socket.Options.SetRequestHeader('Authorization',('Bearer '+$Authorization))}
    [void]$socket.ConnectAsync([Uri]($Origin.Replace('http:','ws:')+$Path),$cancel.Token).GetAwaiter().GetResult()
    return $socket
}
function SendRelayFrame($Socket,[byte[]]$Bytes){
    [void]$Socket.SendAsync([ArraySegment[byte]]::new($Bytes),[Net.WebSockets.WebSocketMessageType]::Binary,$true,$cancel.Token).GetAwaiter().GetResult()
}
function ReadRelayFrame($Socket){
    $bytes=[byte[]]::new(1024)
    $received=$Socket.ReceiveAsync([ArraySegment[byte]]::new($bytes),$cancel.Token).GetAwaiter().GetResult()
    if(!$received.EndOfMessage -or $received.MessageType -ne [Net.WebSockets.WebSocketMessageType]::Binary){throw 'Expected one bounded binary frame'}
    return ,([byte[]]$bytes[0..($received.Count-1)])
}
function VoiceDenied([string]$Path,[string]$Authorization){
    $probe=[Net.WebSockets.ClientWebSocket]::new()
    try{
        $probe.Options.CollectHttpResponseDetails=$true
        foreach($entry in $headers.GetEnumerator()){$probe.Options.SetRequestHeader($entry.Key,$entry.Value)}
        if($Authorization){$probe.Options.SetRequestHeader('Authorization',('Bearer '+$Authorization))}
        try{[void]$probe.ConnectAsync([Uri]($Origin.Replace('http:','ws:')+$Path),$cancel.Token).GetAwaiter().GetResult();return $false}
        catch{return [int]$probe.HttpStatusCode -eq 403}
    }finally{$probe.Dispose()}
}
try{
    $room=Invoke-RestMethod -Uri ($Origin+'/v1/rooms') -Method Post -Headers $headers -ContentType 'application/json' -Body (@{version='0.1.0-demo.4';invitation=('EM2-'+('A'*58))}|ConvertTo-Json -Compress) -TimeoutSec 5
    $route='/v1/rooms/'+$room.code
    $gameHost=NewRelaySocket ($route+'/host') $room.hostKey
    $gameGuest=NewRelaySocket ($route+'/join')
    $admitted=ReadRelayFrame $gameHost
    if($admitted.Count -ne 2 -or $admitted[0] -ne 1 -or !$admitted[1]){throw 'No admitted game channel'}
    $channel=$admitted[1]
    $ticket=@($gameGuest.HttpResponseHeaders['X-Beetleworx-Media-Token'])[0]
    $ticketChannel=@($gameGuest.HttpResponseHeaders['X-Beetleworx-Media-Channel'])[0]
    if($ticket -notmatch '^[a-f0-9]{64}$' -or [int]$ticketChannel -ne $channel){throw 'Media credential is not bound to the actual gameplay channel'}
    if(!(VoiceDenied ($route+'/voice') '') -or !(VoiceDenied ($route+'/voice') ('0'*64))){throw 'Unauthenticated media was admitted'}
    $voiceHost=NewRelaySocket ($route+'/voice') $room.hostKey
    $voiceGuest=NewRelaySocket ($route+'/voice') $ticket
    SendRelayFrame $voiceGuest ([byte[]]@(2,0,17,31,57,83))
    $audio=ReadRelayFrame $voiceHost
    if(($audio -join ',') -ne ('2,'+$channel+',17,31,57,83')){throw 'Guest media identity or bytes changed'}
    SendRelayFrame $voiceHost ([byte[]]@(2,$channel,83,57,31,17))
    $audio=ReadRelayFrame $voiceGuest
    if(($audio -join ',') -ne '2,0,83,57,31,17'){throw 'Host media route or bytes changed'}
    $otherRoom=Invoke-RestMethod -Uri ($Origin+'/v1/rooms') -Method Post -Headers $headers -ContentType 'application/json' -Body (@{version='0.1.0-demo.4';invitation=('EM2-'+('B'*58))}|ConvertTo-Json -Compress) -TimeoutSec 5
    $otherRoute='/v1/rooms/'+$otherRoom.code
    $otherHost=NewRelaySocket ($otherRoute+'/host') $otherRoom.hostKey
    if(!(VoiceDenied ($otherRoute+'/voice') $ticket)){throw 'A media credential crossed room ownership'}
    [void]$voiceGuest.CloseAsync([Net.WebSockets.WebSocketCloseStatus]::NormalClosure,'',$cancel.Token).GetAwaiter().GetResult()
    SendRelayFrame $gameGuest ([byte[]]@(2,0,21,43,65,87))
    $game=ReadRelayFrame $gameHost
    if(($game -join ',') -ne ('2,'+$channel+',21,43,65,87')){throw 'Voice closure disrupted gameplay transport'}
    SendRelayFrame $gameHost ([byte[]]@(4,0,0))
    $locked=$false
    for($attempt=0;$attempt -lt 10 -and !$locked;$attempt++){
        try{$null=Invoke-RestMethod -Uri ($Origin+$route) -Headers $headers -TimeoutSec 5}
        catch{if([int]$_.Exception.Response.StatusCode -eq 409){$locked=$true}else{throw}}
        if(!$locked){Start-Sleep -Milliseconds 50}
    }
    if(!$locked){throw 'Game roster did not lock'}
    $voiceGuest=NewRelaySocket ($route+'/voice') $ticket
    SendRelayFrame $voiceGuest ([byte[]]@(2,0,22,44,66,88))
    $audio=ReadRelayFrame $voiceHost
    if(($audio -join ',') -ne ('2,'+$channel+',22,44,66,88')){throw 'An existing player could not reconnect voice after game start'}
    $replacement=NewRelaySocket ($route+'/voice') $ticket
    SendRelayFrame $replacement ([byte[]]@(2,0,33,55,77,99))
    $audio=ReadRelayFrame $voiceHost
    if(($audio -join ',') -ne ('2,'+$channel+',33,55,77,99')){throw 'Voice replacement did not preserve admitted routing'}
    [void]$gameGuest.CloseAsync([Net.WebSockets.WebSocketCloseStatus]::NormalClosure,'',$cancel.Token).GetAwaiter().GetResult()
    $revoked=$false
    for($attempt=0;$attempt -lt 10 -and !$revoked;$attempt++){
        $revoked=VoiceDenied ($route+'/voice') $ticket
        if(!$revoked){Start-Sleep -Milliseconds 50}
    }
    if(!$revoked){throw 'Departed gameplay owner retained media authorization'}
    [ordered]@{unauthenticated_rejected=$true;game_channel_bound_ticket=$true;cross_room_credential_rejected=$true;bidirectional_media_bytes=$true;voice_close_retains_game=$true;voice_reconnect_after_start=$true;media_replacement=$true;game_departure_revokes_voice=$true;scope='Local Cloudflare runtime with real WebSockets; opaque media bytes only, not native encryption, microphone or gameplay'}|ConvertTo-Json
}finally{
    foreach($socket in $connections){$socket.Dispose()}
    $cancel.Dispose()
}
