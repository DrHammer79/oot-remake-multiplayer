// The crash relay Worker (relay.js has the rules; docs/problem-reports.md, "Owner setup", deploys it). The rate limit
// lives in one Durable Object (RateLimiter), so every Worker instance counts in the same window. Nothing is logged.
import { handle, Limiter, reply } from './relay.js';

export class RateLimiter {
    constructor(state) {
        this.limiter = new Limiter(state.storage);
        this.storage = state.storage;
    }

    async fetch(request) {
        const { ip } = await request.json();
        return Response.json(await this.limiter.take(ip));
    }

    // The window's end: its salt and address hashes are deleted (a request after it starts a new window).
    async alarm() {
        await this.storage.deleteAll();
    }
}

export default {
    async fetch(request, env) {
        try {
            const stub = env.LIMITER.get(env.LIMITER.idFromName('relay'));
            const limiter = {
                async take(ip) {
                    const res = await stub.fetch('https://limiter/take', { method: 'POST', body: JSON.stringify({ ip }) });
                    return res.json();
                },
            };
            return await handle(request, env, { limiter });
        } catch {
            return reply(500, 'error');
        }
    },
};
