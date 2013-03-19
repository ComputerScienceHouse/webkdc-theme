Contest: Design CSH's new WebKDC Templates + Apache Error Pages
============

Think the current [members login site](http://webauth.csh.rit.edu) looks lame and old?

Think that the templates need updating anyway for Stanford WebAuth 4.x series?

Think that our current pages don't work well on portrait-mode phone browsers?

Think our all-too-often-encountered apache-standard web error pages are lame, boring, and not cool enough for our organization?

Well, you're right!

# Getting Started #

Edit the generic templates in weblogin/templates (and images in weblogin/images).
If you don't know or don't want to use git (I see you there, art-types), simply click the "zip" link at the top of the GitHub page to get this as a zip file.

## Strict Requirements ##

* Page must be written in static HTML + JS, no dynamic whatsits and doodads.
* Page may use Javascript, but **must not** _require_ it. (It is extremely common to browse to our login pages with derpy non-JS browsers; think about registering xboxes/kindles/whatever on CSH's network, for instance)
* Page must keep the same form actions, etc.
* Links/images/etc must be relative paths. (for sanity/portability, and prevents mixed content SSL)

## Suggested Requirements ##

* Page should degrade nicely and be usable on small portrait-mode devices (i.e. cell phones); our current site fails at this. But it'd be nice if it didn't.


# Submissions #

Submissions may either exist as pull-requests to this repository, as a link in a news post reply to my own, or an email to clockfort@csh.rit.edu.

### Judging ###

Submissions will be judged by peers, and a final decision will be made using the benevolent dictator model, by Clockfort, in about a month (2013-04-18), depending on responses/quality of submitted work.

## Also Accepting ##

* Custom 404 / 500-series error pages. Maybe a cute little fail-whale-esque thing? This one is pretty free form, just keep in mind that it's very public, so it can't be too obscene. No dynamic content allowed, and any included content must have protocol-agnostic links to avoid mixed content warnings.
