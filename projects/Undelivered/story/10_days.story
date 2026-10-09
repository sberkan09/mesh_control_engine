# Undelivered — Days 2 to 6: the day loop. Mornings alternate pier (Hasan) and café (Mira),
# then the drawer, the delivery, the afternoon, the night journal.
# aux0 = the letter delivered yesterday (0 = none); the morning reads it and clears it.

@label next_day
@set day +1
@if day >= 7 -> day7
@if day == 3 -> morning_cafe
@if day == 5 -> morning_cafe
@jump morning_pier

# ---------------------------------------------------------------- mornings at the pier
@scene morning_pier bg=pier time=morning lut=neutral music=harbour ambience=sea
@if day == 2 -> mp_day2
@if day == 4 -> mp_day4
@jump mp_day6

@label mp_day2
Tuesday. No ferry today, so the pier is Hasan's: he sits on the bollard with a flask and the
  timetable he no longer needs, and watches Yusuf's one working boat go out.
@show hasan center neutral
Hasan: Slept in her chair, I hear. The bread was Feride's. The winding of the clock is me, on
  Mondays and Thursdays, and you can take that over, since you're here.
Hasan: Found what the little key opens?
Deniz:
  > "Letters. She kept people's letters." {honest} -> mp2_tell
  > "Not yet." {first} -> mp2_quiet

@label mp2_tell
He is quiet long enough that a gull lands between you.
Hasan(troubled): Kept. Not lost.
Hasan: Then you've a week of walking in front of you, and I'd start with the oldest, because old
  men die on this island without warning and leave the young to be sorry.
@set trust.hasan +1
@jump mp_end

@label mp2_quiet
Hasan: Then find out. A thing locked for that long wants either opening or burying, and you don't
  look like you brought a spade.
@jump mp_end

@label mp_day4
Thursday, and the ferry's in for an hour, so the pier is crates and shouting and a woman from the
  mainland with a clipboard photographing the bollards, for reasons nobody will explain to her.
@show hasan center neutral
@if aux0 == 1 -> mp4_yusuf
@if aux0 == 2 -> mp4_feride
@if aux0 == 3 -> mp4_halil
@if aux0 == 4 -> mp4_hasan
@if aux0 == 5 -> mp4_cemil
Hasan: Nothing to report. The island has talked about you for two days and run out of things to
  say, which is a record.
@jump mp_end
@label mp4_yusuf
Hasan: Yusuf Reis was at the pier at six this morning asking when the post goes out. In thirty
  years he's never once asked when the post goes out.
@jump mp_end
@label mp4_feride
Hasan(troubled): Feride didn't open the café yesterday afternoon. First time since the funeral.
  Mira ran it on her own and burned the rice. Whatever you took up there, go carefully.
@jump mp_end
@label mp4_halil
Hasan: Light was late coming on last night. Four minutes. Halil's never been four minutes late in
  his life. The whole island noticed and nobody will say so.
@jump mp_end
@label mp4_hasan
@expr hasan warm
Hasan(warm): You know what you gave me. I'm not going to talk about it on a pier with that woman
  photographing my bollards. But I slept, which I hadn't, and that's yours.
@jump mp_end
@label mp4_cemil
Hasan(sharp): Cemil's been on the telephone in the grocery since dawn with the door shut. Last time
  he shut that door was the coup.
@jump mp_end

@label mp_day6
Saturday. The ferry is not due, but Hasan is at the pier anyway, in a clean shirt, scrubbing a
  boat that will be scrapped on Monday.
@show hasan center neutral
@if kemal_boat -> mp6_boat
Hasan: Last whole day. I've been thinking about what I'll do with my hands. Forty years of a wheel
  and then a flat in Çeşme with a balcony the size of this bollard.
@if delivered.4 -> mp6_hasan_letter
Hasan: Don't answer that. It wasn't a question, it was an old man practising for a balcony.
@jump mp_end
@label mp6_hasan_letter
@expr hasan warm
Hasan(warm): Or not Çeşme. I've a letter in my pocket that's made me reconsider the geography.
@jump mp_end
@label mp6_boat
@sfx motor
There is a second boat in the harbour. Blue, mainland registration, a man of seventy at the
  tiller who looks like Yusuf would look if Yusuf had slept well for forty years.
Hasan(open): That's Kemal. Kemal Reis, from Çeşme. Hasn't set foot here since the year I started.
  Yusuf's standing on the slip like a man watching a ghost tie up.
Hasan: It's a good boat. You could run a line on a boat like that. Twice a week, people and post,
  if someone had the papers and someone had the nerve.
@jump mp_end

@label mp_end
@set aux0 =0
@hide all
@jump drawer_hub

# ---------------------------------------------------------------- mornings at the café
@scene morning_cafe bg=cafe time=morning lut=warm music=cafe ambience=cafe
@if day == 5 -> mc_day5

Wednesday. The café before it opens: chairs still up, the radio on low, Mira wiping a table that
  is already clean because her mother is behind the kitchen door.
@show mira center neutral
@if aux0 == 1 -> mc3_yusuf
@if aux0 == 2 -> mc3_feride
@if aux0 == 3 -> mc3_halil
@if aux0 == 4 -> mc3_hasan
@if aux0 == 5 -> mc3_cemil
Mira: Everyone's saying you've got something from the post office. Cemil says it's keys, Hasan
  says it's letters, Selin says it's none of our business, which means she knows.
@jump mc_day3_choice
@label mc3_yusuf
Mira(open): Yusuf came in last night and sat for an hour and didn't drink his tea. Then he asked
  Mum if she had writing paper. Yusuf Reis. Writing paper.
@jump mc_day3_choice
@label mc3_feride
@expr mira troubled
Mira(troubled): She hasn't said a word since you left. Not to me, not to the radio. She sang,
  though, last night, in the kitchen, with the door shut. I haven't heard her sing since I was ten.
Mira: What did you bring her, Deniz?
@jump mc_day3_choice
@label mc3_halil
Mira: Selin was in at seven looking like she'd been up all night. Said her father had gone up to
  the gallery with a bottle and wouldn't come down. Said it was your doing and then said sorry.
@jump mc_day3_choice
@label mc3_hasan
Mira(warm): Hasan came in and ordered the breakfast. The whole breakfast. Eggs. He never has the
  eggs. He sat where he could see the pier and smiled at it like it owed him money.
@jump mc_day3_choice
@label mc3_cemil
Mira(sharp): The muhtar was here at closing talking to Mum about the cove. The cove. He wanted to
  know what the café would charge a hotel for breakfasts. Mum threw a cloth at him.
@jump mc_day3_choice

@label mc_day3_choice
@expr mira open
Mira(open): You said you'd be here Thursday. It's Wednesday. I'm telling you anyway, because I
  can't carry it another day. There's a place for me. İzmir. The conservatory. Voice. I auditioned
  in April, when I was supposed to be at the dentist.
Mira: It starts in October. If I say yes I have to say yes by Sunday, from a post office that
  shuts on Sunday, on an island whose ferry stops on Sunday. The timing's a joke. Say something.
Deniz:
  > "Take it." {leave} -> mc3_take
  > "What does your mother say?" {first} -> mc3_mother
  > "Then say yes. I'll post it myself." {write} -> mc3_post

@label mc3_take
Mira(warm): You don't even hesitate. God. You sound like her, you know. Nermin. She'd have said
  take it and then hidden the acceptance under the counter.
@flag mira_knows_conservatory
@set trust.mira +1
@jump mc_end

@label mc3_mother
Mira(troubled): She doesn't know. She can't know. She gave up her whole life for this café and if
  I tell her I'm leaving it for singing, of all things, for the thing she gave up, she'll smile and
  make tea and never be the same.
@flag mira_knows_conservatory
@jump mc_end

@label mc3_post
Mira(open): You'd do that? Then I'll write it tonight. No. Friday. Let me have one more day of not
  having done it.
@flag mira_knows_conservatory
@set trust.mira +2
@jump mc_end

@label mc_day5
Friday. Rain, the first in a month, and the café full of people pretending they came in for the
  rain. Mira makes you tea without asking and sits, which she is not supposed to do when it's busy.
@show mira center neutral
@if aux0 == 1 -> mc5_yusuf
@if aux0 == 2 -> mc5_feride
@if aux0 == 3 -> mc5_halil
@if aux0 == 4 -> mc5_hasan
@if aux0 == 5 -> mc5_cemil
Mira: Quiet day yesterday, by your standards. People are almost disappointed.
@jump mc_day5_tail
@label mc5_yusuf
Mira: Yusuf's letter went out on the Thursday boat. Hasan carried it up to the bridge himself
  instead of putting it in the sack. I don't think I've ever seen Hasan carry a letter.
@jump mc_day5_tail
@label mc5_feride
@expr mira troubled
Mira(troubled): Mum and I talked. Properly. First time. She knows about İzmir. She knew before I
  said it, apparently, because she's my mother and because, she says, she once had a letter like
  that herself and never got it. She wouldn't say more.
@jump mc_day5_tail
@label mc5_halil
Mira: Selin says the light came on at the right minute last night, and that her father's stopped
  drinking and started painting the gallery rail, which is either very good or very bad.
@jump mc_day5_tail
@label mc5_hasan
Mira(warm): Hasan asked Mum what the café would do for a wedding. Then he said he meant a funeral.
  Then he said he meant nothing and paid for the whole room's tea and left.
@jump mc_day5_tail
@label mc5_cemil
Mira(sharp): There was a man off the Thursday boat in a suit, measuring the cove with a wheel.
  Cemil walked beside him with a face like a man at his own trial.
@jump mc_day5_tail

@label mc_day5_tail
@if feride_blessing -> mc5_blessing
@if mira_knows_conservatory -> mc5_knows
Mira(open): Two days. I still haven't told you my thing. I will. I'm a coward, that's all.
@jump mc_end
@label mc5_knows
Mira(open): Two days. The letter's written. It's in my apron. I keep not posting it.
@jump mc_end
@label mc5_blessing
@expr mira warm
Mira(warm): She said go. My mother said go, Deniz. She said it with the dough still on her hands,
  like it was a thing you say every day. I posted the letter this morning. Hasan has it.
@set trust.mira +1
@jump mc_end

@label mc_end
@set aux0 =0
@hide all
@jump drawer_hub

# ---------------------------------------------------------------- the drawer
# @drawer hands control to the drawer screen; it sets drawer_letter (1..6, 0 = none) and
# drawer_action (0 = deliver sealed, 1 = read it first, 2 = left for now, 3 = nothing left).
@scene drawer_hub bg=drawer time=noon lut=sepia music=drawer ambience=clock
@drawer
@if drawer_letter == 1 -> deliver1
@if drawer_letter == 2 -> deliver2
@if drawer_letter == 3 -> deliver3
@if drawer_letter == 4 -> deliver4
@if drawer_letter == 5 -> deliver5
@if drawer_letter == 6 -> deliver6
@if drawer_action == 3 -> drawer_empty
You shut the drawer on them for today. They have waited this long. You tell yourself that twice,
  and the second time it almost works.
@set alone_days +0
@jump afternoon_choice

@label drawer_empty
Six envelopes gone from the drawer and one left, with no name on it. It is not time for that one.
  You lock it anyway, out of habit, out of hers.
@jump afternoon_choice

# ---------------------------------------------------------------- the afternoon
@scene afternoon_choice bg=postoffice time=noon lut=neutral music=none ambience=clock
The afternoon is yours. The island is small enough that whoever you choose will know you chose.
Deniz:
  > "The café roof, with Mira." {leave} -> aft_mira
  > "The lighthouse, with Selin." {stay} -> aft_selin
  > "The slip, with Yusuf Reis." {first} -> aft_yusuf
  > "Alone, in the post office." {shut} -> aft_alone

@label aft_mira
@if mira_days == 0 -> mira_1
@if mira_days == 1 -> mira_2
@if mira_days == 2 -> mira_3
@if mira_days == 3 -> mira_4
@jump mira_5
@label aft_selin
@if selin_days == 0 -> selin_1
@if selin_days == 1 -> selin_2
@if selin_days == 2 -> selin_3
@if selin_days == 3 -> selin_4
@jump selin_5
@label aft_yusuf
@if yusuf_days == 0 -> yusuf_1
@if yusuf_days == 1 -> yusuf_2
@if yusuf_days == 2 -> yusuf_3
@if yusuf_days == 3 -> yusuf_4
@jump yusuf_5
@label aft_alone
@if alone_days == 0 -> alone_1
@if alone_days == 1 -> alone_2
@if alone_days == 2 -> alone_3
@if alone_days == 3 -> alone_4
@jump alone_5

# ---------------------------------------------------------------- the night
@scene night bg=postoffice time=night lut=sepia music=drawer ambience=clock
@hide all
@if day == 6 -> night_six
You write the day down in the back of her ledger, under the last stamp she sold, because it
  seems like the place for it.
@call journal_line
@journal
@jump next_day

@label night_six
Saturday night. The last. The boxes are packed except the one on the counter, and the drawer has
  one envelope in it, and you have run out of reasons.
@call journal_line
*for whoever opens this drawer*
@sfx envelope
@letter 7
@flag letter7_read
@flag read.7
You read it twice. The second time you read it in her voice, and that is the time you cry, with
  the clock going, in a post office that will not be one on Monday.
@if nermin_blamed -> n6_blamed
You put the letter in your coat, where she kept the key. Whatever happens on the pier tomorrow,
  it happens with this in your pocket.
@jump n6_end
@label n6_blamed
You said she had no right. You still think it. But you understand now that she knew it too, every
  day for forty years, and kept the drawer anyway, and that is a different thing from not knowing.
@label n6_end
@journal
@jump next_day

# One paragraph in Deniz's hand, from the day's flags.
@label journal_line
@if aux0 == 1 -> jl_1
@if aux0 == 2 -> jl_2
@if aux0 == 3 -> jl_3
@if aux0 == 4 -> jl_4
@if aux0 == 5 -> jl_5
@if aux0 == 6 -> jl_6
Nothing delivered today. The drawer is as it was. I walked, and talked, and the island let me,
  and I don't know if that was mercy or patience running out.
@return
@label jl_1
Took Yusuf Reis his brother's letter. Twenty-three years. He read it with his glasses on upside
  down and didn't notice, and I didn't tell him.
@return
@label jl_2
Took Feride the conservatory's letter. She was twenty-six when it came. She's fifty-two. I don't
  know what I expected her to do with those years in one afternoon.
@return
@label jl_3
Took Halil the directorate's approval. He'd have gone in 1989, with Selin, to a light on the
  mainland with a town behind it. He didn't say thank you and I didn't expect him to.
@return
@label jl_4
Took Hasan her letter. Hers. I stood on the slip while he read it on the bridge and I have never
  felt so much like a trespasser in my life.
@return
@label jl_5
Took the muhtar the hotel's offer. He read it twice and then looked at the cove through the shop
  window like a man pricing his own kidney.
@return
@label jl_6
Opened the one with my name on it. Hamburg, 1983. I was eight. I'm not going to write about it here.
@return
