# Undelivered — Day 1, Monday: Arrival.
# Second person, present tense. Deniz speaks in the choices and rarely otherwise.

@scene start bg=pier time=evening lut=warm music=harbour ambience=sea
@set day =1
@cutscene ferry

The ferry is late, which on Kuzgun means it is on time. You stand at the rail with one bag and
  the smell of diesel and salt, and the island comes up out of the haze the way it always did: the
  lighthouse first, then the white of the café, then the dark cut of the cove.

Nobody is waiting on the pier for you. You told nobody the day. There is a crate of bread, two
  crates of tomatoes, a sack of post, and a man in a blue cap who has been master of this boat
  since before you were born.

@show hasan center neutral
Hasan: Nermin's grandchild. You've grown into her nose.
Hasan: Don't look like that. It's a good nose. It got letters where they were going for forty
  years.

You take the rope he throws you because your hands remember it from summers, and tie it off on
  the bollard without thinking.

Hasan(warm): See. The island doesn't forget you, even if you forget the island.

Deniz:
  > "I didn't forget. I was at university." {first} -> d1_hasan_uni
  > "She would have come to meet the boat." {honest} -> d1_hasan_nermin
  > "Is it true about Sunday?" {hard} -> d1_hasan_sunday

@label d1_hasan_uni
Hasan: İzmir. I know. She read your letters to the whole café. The one about the professor with
  the pigeons, she read twice.
You did not know she did that. You find you have to look at the water for a moment.
@jump d1_hasan_after

@label d1_hasan_nermin
Hasan(troubled): She came to meet every boat for forty years, whether there was anyone on it or
  not. I used to think she was waiting for the post. Then I thought she was waiting for someone.
Hasan: Then I stopped thinking about it, which is the only way to run a ferry.
@set trust.hasan +1
@jump d1_hasan_after

@label d1_hasan_sunday
Hasan(sharp): It's true. Sunday, the ten o'clock, and then the Kuzgun II goes to Çeşme to be
  scrapped or sold, and I go home to a flat I have never slept in.
Hasan: And no, before you ask. There will be no speech. I have been asked for a speech by the
  muhtar, the imam, and a woman from the radio in İzmir. There will be no speech.
@jump d1_hasan_after

@label d1_hasan_after
Hasan: Go on. The café's open. Feride will feed you whether you want it or not, and the keys are
  with the muhtar, and the post office smells of her still, so take a breath before you go in.
@sfx horn
He goes back up to his bridge. The gangway is pulled. You are, as of this moment, on the island
  for a week, with no boat until Wednesday and a post office to fold into boxes.
@hide all

@scene d1_cafe bg=cafe time=evening lut=warm music=cafe ambience=cafe
The café is three tables outside and four in, a counter, a radio, a wall of photographs of boats
  that no longer exist. Feride is in the kitchen. Mira is on the counter, which she has been told
  not to sit on since she was six.
@show mira center warm
Mira(warm): Deniz. Deniz. You absolute coward, you didn't write.
Mira: Sit. No, there. Where I can see you. Mum! It's Deniz!
@show feride left warm
Feride(warm): I can see it's Deniz, I have eyes. Sit, child. You're thin. They don't feed you in
  İzmir?
You are fed. Bread, white cheese, olives from the terrace, tea in the glass with the chipped rim
  that has been your glass since you were eight. Feride asks about your mother, and your grades,
  and does not ask about your father, which is how you know she has thought about asking.
Feride: I'll leave you two. The dough won't knead itself.
@hide feride
@expr mira open
Mira(open): So. How long?
Deniz:
  > "Until Sunday. Until the ferry." {first} -> d1_mira_sunday
  > "As long as it takes to pack her up." {honest} -> d1_mira_pack

@label d1_mira_sunday
Mira(troubled): Everyone's leaving on Sunday. It's going to be like a wedding where the bride runs
  off. Hasan says there'll be a hundred people on the pier and he'll have to count them.
@jump d1_mira_hint

@label d1_mira_pack
Mira(troubled): Pack her up. God. She used to say she'd die at that counter, and she nearly did,
  you know. Cemil found her in the back room with a stamp in her hand.
Mira: Sorry. That was a terrible thing to say first.
@set trust.mira +1
@jump d1_mira_hint

@label d1_mira_hint
She picks at the edge of the counter. There is something in her pocket that she keeps touching
  through the cloth, the way you touch a tooth that hurts.
Mira(open): Can I tell you something later? Not now. Later in the week. When I know.
Deniz:
  > "Tell me now." {hard} -> d1_mira_now
  > "Whenever you're ready." {first} -> d1_mira_later

@label d1_mira_now
Mira(sharp): No. Because if I say it out loud in this room it becomes a thing Mum can hear, and I
  haven't decided. Later. Promise you'll still be here Thursday.
You promise. You are not sure what you have promised.
@jump d1_cafe_end

@label d1_mira_later
Mira(warm): Thanks. You were always better at waiting than me. It's annoying.
@set trust.mira +1
@flag mira_asked
@jump d1_cafe_end

@label d1_cafe_end
Mira: Go. Cemil shuts at seven and he'll make you listen to the whole ferry story if you're late.
@hide all

@scene d1_grocery bg=grocery time=evening lut=warm music=none ambience=clock
The grocery is also the muhtar's office, which is to say it has a second table with a stamp on it
  and a framed photograph of the Prime Minister that is two Prime Ministers out of date.
@show cemil center neutral
Cemil: Deniz. Condolences, again, in person. She was the best of us, and I say that as someone
  she called a donkey to my face on at least four occasions.
Cemil: The keys. Front door, back door, the counter drawers, the safe which is empty, and this one.
He holds up a small brass key on a loop of red thread, and then does not quite hand it over.
Cemil(troubled): This one I don't know. It isn't a post office key. It was in her coat. The
  directorate's man asked for all keys and I said I had given him all keys, so now I have lied to
  the state for her, which she'd have enjoyed.
Deniz:
  > "I'll find what it opens." {first} -> d1_cemil_find
  > "You could have kept it. She'd have wanted you to have something." {honest} -> d1_cemil_keep

@label d1_cemil_find
Cemil: Find it before Sunday. After Sunday the building is theirs and what's in it is theirs, and
  I don't want a man from Ankara finding whatever Nermin Hanım kept behind a lock.
@jump d1_cemil_end

@label d1_cemil_keep
Cemil(warm): I kept her bad temper. That's enough of her for one man.
Cemil: Take it. Find it before Sunday, and if it opens something a man from Ankara shouldn't see,
  come to me first.
@set trust.cemil +1
@jump d1_cemil_end

@label d1_cemil_end
Cemil: The ferry. You'll have heard. I've written to everyone. The ministry, the governor, two
  newspapers, a man I was at school with who is now, God help us, a deputy. Nothing. Sixty thousand
  a year and they won't find it. There are villages on the mainland that get more for a fountain.
Cemil(sharp): There was a way to keep it once. There isn't now. Go. Sleep. You look like her when
  she was tired, and she was tired a lot, at the end.
@hide all

@scene d1_postoffice bg=postoffice time=night lut=sepia music=drawer ambience=clock
You let yourself in. The clock is still going; someone has been winding it. Ink, paper, the
  paraffin heater, the particular dust of a room where nothing has been thrown away since 1953.
  Her cardigan is on the back of the chair. You do not touch it yet.
The counter has three drawers. The front two open. The third, the deep one on the left, under
  the stamp pads, does not, and there is a brass lock in it the size of a thumbnail.
@sfx envelope
The key turns as if it has been turned every day of its life.
Seven envelopes. Oldest at the bottom, the paper gone the colour of tea. The top one is new,
  this spring's, and has no name on it, only, in her hand, *for whoever opens this drawer*.
You lay the other six out on the counter under the lamp, as she must have, more than once.
Yusuf Reis, from Çeşme, 1971. Feride, from a conservatory in İstanbul, 1978. Halil, the keeper,
  from the directorate, 1989. Kaptan Hasan, in her own hand, 1986. Cemil, from a company, 1992.
And one addressed to you. Hamburg. 1983. Your father's writing, which you know from exactly two
  birthday cards.
None of them has been opened. All of them have been kept.
Deniz:
  > "You kept them. Why did you keep them?" {first} -> d1_night_why
  > "You had no right." {hard} -> d1_night_right
  > "You must have been so frightened." {honest} -> d1_night_afraid

@label d1_night_why
You say it to the cardigan, which is unfair to the cardigan. The clock goes on. Somewhere out
  past the breakwater a boat you cannot see changes gear.
@jump d1_night_end

@label d1_night_right
You hear yourself and you sound like your mother on the telephone, and you stop, because that is
  not a voice you want in this room.
@flag nermin_blamed
@jump d1_night_end

@label d1_night_afraid
The island is very quiet at night. You think of her at this counter at nineteen, at thirty, at
  sixty, with the harbour emptying one ferry at a time, and a letter in front of her that said
  *come*.
@flag nermin_defended
@jump d1_night_end

@label d1_night_end
You put the seventh envelope back on top, unopened. Not tonight. You lock the drawer and sleep in
  her chair with the key in your fist, and in the morning there is bread on the step, because this
  is still that kind of island, for six more days.
@journal
@jump next_day
