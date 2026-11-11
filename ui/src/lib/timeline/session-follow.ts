// Which session the Timeline preview shows: the page's own authority over `timeline.get`. With no id
// asked for, a reply's `session` may be the newest one on disk (the engine keeps that fallback for the
// deck's filmstrip), so only `current_session` - the writer's own - may ever be adopted here. A stored
// session opens when its id was asked for, never by accident.

/** What the page does about the session the writer is on now. */
export type FollowAction = 'open-current' | 'wait' | 'keep' | 'announce'

/** Everything the decision needs, as the page knows it at one poll. */
export interface FollowInput {
  /** The session on screen; '' when nothing is shown. */
  shownId: string
  /** `timeline.get.current_session`: the writer's session, '' when no write is open. */
  currentSession: string
  /** The owner asked for the shown stored session by its id (a launch on one): it is what they came to look at. */
  chosen: boolean
  /** The owner is working on what is on screen: a frozen view or a clip. Not merely a paused picture: the picture of a
   *  session that has ended is paused too, and that is the very one that must give way. */
  reviewing: boolean
  /** LIVE was pressed and the current session is not readable yet. */
  pendingLive: boolean
}

/**
 * `wait` is the normal state for a session that has ended, so a stored session is never reopened behind
 * the owner: no current session means nothing new to show. When the writer starts a new session, the one on
 * screen has stopped, and it is replaced - a stopped session never stays up inside a new one - unless the owner
 * is working on it (announced, kept) or asked for that stored session by its id (kept). A pressed LIVE
 * (`pendingLive`) is not "on its own".
 */
export function nextSession(input: FollowInput): FollowAction {
  if (!input.currentSession) return 'wait'
  if (input.currentSession === input.shownId) return 'keep'
  // Nothing on screen: there is no work to lose, and the writer's session is the one to show.
  if (!input.shownId) return 'open-current'
  // LIVE was pressed, so the owner already chose the writer's session; a retry must not wait on a review.
  if (input.pendingLive) return 'open-current'
  if (input.reviewing) return 'announce'
  return input.chosen ? 'keep' : 'open-current'
}
