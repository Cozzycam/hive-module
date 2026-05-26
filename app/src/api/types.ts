// ---- Colony Snapshot (GET /api/v1/colonies/:id) ----

export interface ColonySnapshot {
  schema: number;
  colony_id: string;
  founded_unix: number;
  now_unix: number;
  world: World;
  population: Population;
  food: Food;
  modules: Module[];
  lilguys?: LilGuySummary[];  // living roster, included in VPS snapshots
}

export interface World {
  tod: TimeOfDay;
  day_of_year: number;
  temperature_c: number;
  humidity_pct: number;
  weather: WeatherType;
  season: Season;
  active_challenges: Challenge[];
}

export interface TimeOfDay {
  phase: 'day' | 'night' | 'dawn' | 'dusk';
  night_factor: number; // 0.0 = full day, 1.0 = full night
  day_progress: number; // 0.0 at sunrise, 1.0 at sunset
}

export type WeatherType =
  | 'clear' | 'partly_cloudy' | 'overcast' | 'fog'
  | 'drizzle' | 'rain' | 'heavy_rain' | 'snow' | 'thunderstorm';

export type Season = 'spring' | 'summer' | 'autumn' | 'winter' | 'indeterminate';

export interface Challenge {
  type: 'heatwave' | 'cold_snap' | 'drought' | 'storm';
  severity: number;
}

export interface Population {
  alive: number;
  dead_total: number;
  by_role: {
    queen: number;
    conker: number;   // total workers = conker; founder is a tag, not a separate count
    founder: number;  // subset of conker — first FOUNDER_COHORT_SIZE hatches
    brood_egg: number;
    brood_seed: number;
  };
}

export interface Food {
  store: number;
  daily_burn: number;
  days_remaining: number;
}

export interface Module {
  id: string;
  role: 'queen' | 'satellite';
  online: boolean;
  faces: {
    north?: string | null;
    south?: string | null;
    west?: string | null;
    east?: string | null;
  };
}

// ---- LilGuy list (queen LAN: GET /api/v1/lilguys) ----

export interface LilGuyListResponse {
  schema: number;
  total: number;
  results: LilGuySummary[];
}

export interface LilGuySummary {
  id: number;
  name: string;
  role: 'conker';       // single caste — founder is a tag, not a role
  founder?: boolean;
  age_days: number;     // computed from lived_ms in firmware
  traits: string[];
  personality?: Personality;
}

// ---- LilGuy detail (queen LAN: GET /api/v1/lilguys/:id) ----

export interface LilGuyDetail {
  schema: number;
  id: number;
  name: string;
  role: 'conker';
  founder?: boolean;
  born_unix: number;
  age_days: number;       // from lived_ms — only counts sim-running time
  died_unix: number | null;
  tended_by: { id: number; name: string } | null;
  personality: Personality;
  traits: string[];
  bonds: Bond[];
  last_pos: { module: string; x: number; y: number };
  event_count: number;
}

export interface Personality {
  work_tempo: number;
  exploration: number;
  route_stickiness: number;
  social_frequency: number;
  food_preference: number;
  hardiness: number;
  learning_rate: number;
}

export interface Bond {
  to: { id: number; name: string };
  strength: number;
}

// ---- Events (GET /api/v1/colonies/:id/events) ----

export interface EventsResponse {
  schema: number;
  results: ColonyEvent[];
  next_since: number;
}

export type EventType =
  | 'hatch' | 'death' | 'role_change'
  | 'food_tap' | 'food_discovered' | 'food_delivered'
  | 'chamber_crossing' | 'milestone' | 'colony_event'
  | 'tended_by_assigned' | 'bond_formed' | 'bond_broken'
  | 'challenge_start' | 'challenge_end' | 'trait_earned';

export interface ColonyEvent {
  tick: number;
  unix: number;
  type: EventType;
  lilguy: number;
  data: Record<string, unknown>;
}

// ---- Colonies list (GET /api/v1/colonies) ----

export interface ColoniesListResponse {
  schema: number;
  colonies: { colony_id: string; last_snapshot_unix: number }[];
}

// ---- Health (GET /api/v1/health) ----

export interface HealthResponse {
  schema: number;
  status: string;
  colonies_count: number;
}

// ---- Data source ----

export type DataSource = 'lan' | 'vps' | 'cache' | 'none';
