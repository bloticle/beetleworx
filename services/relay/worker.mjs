import { DurableObject } from 'cloudflare:workers';

const alphabet = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';
const lifetime = 6 * 60 * 60 * 1000;
const reply = (value, status = 200) => Response.json(value, {
  status, headers: { 'Cache-Control': 'no-store' },
});
function random(length, characters = alphabet) {
  const bytes = crypto.getRandomValues(new Uint8Array(length));
  // Both alphabets have power-of-two lengths; no modulo bias.
  return Array.from(bytes, n => characters[n % characters.length]).join('');
}
function version(value) {
  if (typeof value !== 'string' || value.length >= 128) return null;
  if (!/^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?$/.test(value)) return null;
  const normalized = value.split('+')[0];
  const prerelease = normalized.split('-').slice(1).join('-');
  if (prerelease.split('.').some(x => /^0[0-9]+$/.test(x))) return null;
  return normalized;
}
function packet(kind, channel, payload = new Uint8Array()) {
  const bytes = new Uint8Array(2 + payload.byteLength);
  bytes[0] = kind; bytes[1] = channel; bytes.set(payload, 2);
  return bytes;
}
async function smallBody(request) {
  if (!request.body) return '';
  const reader = request.body.getReader(), chunks = []; let length = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    if ((length += value.byteLength) > 2048) { await reader.cancel(); return null; }
    chunks.push(value);
  }
  const bytes = new Uint8Array(length); let at = 0;
  for (const chunk of chunks) { bytes.set(chunk, at); at += chunk.length; }
  return new TextDecoder('utf-8', { fatal: true }).decode(bytes);
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname === '/health' && request.method === 'GET') return reply({ protocol: 1 });
    const route = url.pathname.match(/^\/v1\/rooms\/([A-HJ-NP-Z2-9]{8})(?:\/(host|join))?$/);
    if (!(url.pathname === '/v1/rooms' && request.method === 'POST') && !route) return reply({ error: 'Not found' }, 404);
    // Cloudflare supplies this header; do not retain or log raw client addresses.
    const ip = request.headers.get('CF-Connecting-IP');
    if (!ip) return reply({ error: 'Missing edge client address' }, 400);
    const day = Math.floor(Date.now() / 86400000);
    const hash = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(`${day}:${ip}`));
    const key = Array.from(new Uint8Array(hash), b => b.toString(16).padStart(2, '0')).join('');
    const admission = await env.ADMISSION.getByName(key).fetch(new Request('https://internal/limit', {
      method: 'POST', body: url.pathname === '/v1/rooms' ? 'create' : 'connect',
    }));
    if (!admission.ok) return admission;
    if (route) return env.ROOMS.getByName(route[1]).fetch(request);
    let body; try { body = await smallBody(request); } catch { return reply({ error: 'Invalid request' }, 400); }
    if (body === null) return reply({ error: 'Request too large' }, 413);
    let options; try { options = JSON.parse(body); } catch { return reply({ error: 'Invalid request' }, 400); }
    if (!options || typeof options !== 'object' || Array.isArray(options)) return reply({ error: 'Invalid request' }, 400);
    const release = version(options.version);
    if (!release || typeof options.invitation !== 'string' || !/^[A-Z0-9-]{20,256}$/.test(options.invitation)) return reply({ error: 'Invalid release or invitation' }, 400);
    for (let attempt = 0; attempt < 4; ++attempt) {
      const code = random(8), hostKey = random(64, '0123456789abcdef');
      const result = await env.ROOMS.getByName(code).fetch(new Request('https://internal/create', {
        method: 'POST', body: JSON.stringify({ version: release, invitation: options.invitation, hostKey, expires: Date.now() + lifetime }),
      }));
      if (result.status === 409) continue;
      if (!result.ok) return result;
      return reply({ protocol: 1, code, hostKey, version: release });
    }
    return reply({ error: 'Could not allocate a room. Retry.' }, 503);
  },
};

export class Admission extends DurableObject {
  async fetch(request) {
    const kind = await request.text(), now = Date.now();
    return this.ctx.blockConcurrencyWhile(async () => {
      const state = await this.ctx.storage.get('limits') || { hour: now, minute: now, creates: 0, connects: 0 };
      if (now - state.hour >= 3600000) { state.hour = now; state.creates = 0; }
      if (now - state.minute >= 60000) { state.minute = now; state.connects = 0; }
      if ((kind === 'create' && state.creates >= 6) || state.connects >= 120) return reply({ error: 'Too many attempts. Try again shortly.' }, 429);
      if (kind === 'create') ++state.creates;
      ++state.connects; await this.ctx.storage.put('limits', state);
      await this.ctx.storage.setAlarm(now + 25 * 3600000);
      return reply({ allowed: true });
    });
  }
  async alarm() { await this.ctx.storage.deleteAll(); }
}

export class RelayRoom extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    this.room = null;
    this.ctx.blockConcurrencyWhile(async () => { this.room = await ctx.storage.get('room'); });
    ctx.setWebSocketAutoResponse(new WebSocketRequestResponsePair('ping', 'pong'));
  }
  host() { return this.ctx.getWebSockets('host')[0]; }
  guests() { return this.ctx.getWebSockets('guest'); }
  async fetch(request) {
    const url = new URL(request.url);
    if (url.pathname === '/create' && request.method === 'POST') {
      if (this.room) return reply({ error: 'Room already exists' }, 409);
      this.room = await request.json();
      await this.ctx.storage.put('room', this.room);
      await this.ctx.storage.setAlarm(this.room.expires);
      return reply({ created: true });
    }
    if (!this.room || this.room.closed || this.room.expires <= Date.now()) return reply({ error: 'This room has closed' }, 410);
    const release = version(request.headers.get('X-Beetleworx-Version'));
    if (release !== this.room.version) return reply({ error: 'Different Beetleworx version', requiredVersion: this.room.version }, 409);
    const role = url.pathname.endsWith('/host') ? 'host' : url.pathname.endsWith('/join') ? 'guest' : null;
    if (role !== 'host' && this.room.joinable === false) return reply({ error: 'This game has already started. Ask the host to create a new lobby.' }, 409);
    if (!role && request.method === 'GET') return this.host()
      ? reply({ protocol: 1, version: this.room.version, invitation: this.room.invitation, players: this.guests().length + 1, capacity: 16 })
      : reply({ error: 'The host is still connecting. Retry shortly.' }, 425);
    if (request.method !== 'GET' || request.headers.get('Upgrade')?.toLowerCase() !== 'websocket') return reply({ error: 'WebSocket required' }, 426);
    if (!role) return reply({ error: 'Not found' }, 404);
    let channel = 0;
    if (role === 'host') {
      if (request.headers.get('Authorization') !== `Bearer ${this.room.hostKey}`) return reply({ error: 'Host authorization failed' }, 403);
      if (this.host()) return reply({ error: 'The host is already connected' }, 409);
    } else {
      if (!this.host()) return reply({ error: 'The host is offline' }, 410);
      if (this.guests().length >= 15) return reply({ error: 'This room is full' }, 409);
      // Never reuse a channel in this room: delayed host data must not reach a
      // different guest after a disconnect. Native Room assigns its own peers.
      channel = (this.room.lastChannel || 0) + 1;
      if (channel > 255) return reply({ error: 'Create a fresh room to admit more guests' }, 409);
      this.room.lastChannel = channel;
      await this.ctx.storage.put('room', this.room);
      if (!this.host() || this.room.closed) return reply({ error: 'The host left' }, 410);
      if (this.room.joinable === false) return reply({ error: 'This game has already started. Ask the host to create a new lobby.' }, 409);
      if (this.guests().length >= 15) return reply({ error: 'This room is full' }, 409);
    }
    const pair = new WebSocketPair();
    this.ctx.acceptWebSocket(pair[1], [role]);
    pair[1].serializeAttachment({ role, channel, at: Date.now(), budget: role === 'host' ? 32 * 1024 * 1024 : 4 * 1024 * 1024 });
    if (role === 'guest') this.host().send(packet(1, channel));
    return new Response(null, { status: 101, webSocket: pair[0] });
  }
  async webSocketMessage(ws, message) {
    const state = ws.deserializeAttachment();
    if (!(message instanceof ArrayBuffer) || message.byteLength < 2 || message.byteLength > 65538) { ws.close(1008, 'Invalid relay frame'); return; }
    const now = Date.now(), limit = state.role === 'host' ? 32 * 1024 * 1024 : 4 * 1024 * 1024;
    state.budget = Math.min(limit, state.budget + Math.max(0, now - state.at) * limit / 1000) - message.byteLength;
    state.at = now;
    if (state.budget < 0) { ws.close(1008, 'Relay rate exceeded'); return; }
    ws.serializeAttachment(state);
    const bytes = new Uint8Array(message), kind = bytes[0], channel = bytes[1];
    if (state.role === 'guest') {
      if (kind !== 2 || channel !== 0 || bytes.length < 3) { ws.close(1008, 'Invalid guest frame'); return; }
      bytes[1] = state.channel;
      try { this.host()?.send(bytes); } catch { ws.close(1011, 'Host disconnected'); }
    } else {
      if (kind === 4 && channel === 0 && bytes.length === 3 && bytes[2] <= 1) {
        this.room.joinable = bytes[2] === 1;
        await this.ctx.storage.put('room', this.room);
        return;
      }
      const guest = this.guests().find(peer => peer.deserializeAttachment().channel === channel);
      if (!guest) return; // A close can cross already queued host data.
      if (kind === 3 && bytes.length === 2) guest.close(1000, 'Game connection closed');
      else if (kind === 2 && bytes.length > 2) { bytes[1] = 0; try { guest.send(bytes); } catch { guest.close(1011, 'Guest disconnected'); } }
      else ws.close(1008, 'Invalid host frame');
    }
  }
  async webSocketClose(ws) { await this.departed(ws); }
  async webSocketError(ws) { await this.departed(ws); }
  async departed(ws) {
    const state = ws.deserializeAttachment();
    if (state.role === 'host') {
      this.room.closed = true;
      await this.ctx.storage.put('room', this.room);
      for (const guest of this.guests()) try { guest.close(1001, 'The host left'); } catch {}
    } else try { this.host()?.send(packet(3, state.channel)); } catch {}
  }
  async alarm() {
    for (const ws of this.ctx.getWebSockets()) try { ws.close(1001, 'Room expired'); } catch {}
    await this.ctx.storage.deleteAll(); this.room = null;
  }
}
