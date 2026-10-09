# Undelivered — Day 7, Sunday: the last ferry, the five endings, the epilogue cards.
# aux_flag_d = Open Water available, aux_flag_e = Lamp Keeper available.

@scene day7 bg=pier time=morning lut=warm music=sunday ambience=sea
@set aux0 =0
Sunday, and the whole island on the pier. A hundred and forty people and their chairs, because
  Feride sent the chairs down, and the imam, and the woman from the radio in İzmir with a
  microphone that Hasan has twice declined to speak into.
The Kuzgun II with her flags up, which she has not had since the republic's fiftieth year. Yusuf's
  boats at the slip. The lighthouse off, because it is day, and because, for once, somebody is
  not up there.
@if kemal_boat -> d7_kemal_note
@jump d7_cast
@label d7_kemal_note
And a blue boat with a mainland registration tied where the fishing boats go, and a crowd round
  it that is not looking at the ferry at all.
@label d7_cast

@show hasan right neutral
Hasan: Ten o'clock. In forty years I've never once gone early and I'll not start today, whatever
  the radio says. Twenty minutes. Say what you've got to say to people, child. That's what the
  twenty minutes are for.

# ---- the island, one by one, from the flags
@show feride left neutral
@if delivered.2 -> d7_feride_yes
Feride: You'll eat on the boat. I've put a bag with Hasan. Don't argue; I've buried a mother and I
  know what a journey wants.
@jump d7_feride_end
@label d7_feride_yes
@if feride_blessing -> d7_feride_bless
Feride(troubled): I've had my letter a week. Sixteen years late and a week. I haven't decided what
  to do with it and I'm fifty-two; I may never. But it's mine. Thank you for that, at least.
@jump d7_feride_end
@label d7_feride_bless
@expr feride warm
Feride(warm): She's got her bag. I've put the letter in it, mine, the conservatory's, so she has
  something to show them if they ask whether it runs in the family. Go on. Say goodbye to her;
  she's at the end of the pier pretending not to cry.
@label d7_feride_end
@hide feride

@show selin left neutral
@if halil_stays_keeper -> d7_selin_keeper
Selin: He's up there. He wouldn't come down; said someone has to watch the light even when it's
  off. I came down. I wanted to see it go from this end, once.
@jump d7_selin_end
@label d7_selin_keeper
@expr selin open
Selin(open): He came down. Look. By the bollard, in his good coat, not talking to anyone. He said
  the keeper should see the island off. He meant me, and he came anyway.
@label d7_selin_end
@hide selin

@show yusuf left neutral
@if kemal_boat -> d7_yusuf_boat
@if delivered.1 -> d7_yusuf_letter
Yusuf: One boat working. Same as Monday. Go on; you've a boat of your own to catch.
@jump d7_yusuf_end
@label d7_yusuf_letter
Yusuf(open): No answer yet from Çeşme. There'll be one. I've decided there'll be one, and at my age
  deciding is most of it.
@jump d7_yusuf_end
@label d7_yusuf_boat
Yusuf(warm): He's staying the week. Kemal. In the house, in Mother's room, snoring like a diesel.
  Cemil's got the papers out on the counter. There'll be a boat Tuesday, child, whatever the state
  says. You can tell İzmir that.
@label d7_yusuf_end
@hide yusuf

@show cemil left neutral
@if cove_sold -> d7_cemil_sold
@if cemil_pressed -> d7_cemil_island
Cemil: No ferry, no hotel, no fountain. But I've written to the deputy again and this time I've
  put Nermin's name on it, and he was in love with her in 1961, so we'll see.
@jump d7_cemil_end
@label d7_cemil_sold
Cemil(troubled): I rang them. The cove's theirs by Christmas and a launch by Easter. Feride's not
  spoken to me since Thursday. The children will have a boat and nowhere to swim. I've done worse
  for the island and I've done better and I'll never know which this was.
@jump d7_cemil_end
@label d7_cemil_island
Cemil(open): The café voted. Eighty-one to fifty-nine against the hotel, with the beach clause read
  out twice. First honest vote this island's had since I was elected, and I was unopposed.
@label d7_cemil_end
@hide cemil

# ---- Return Address: the reply comes before the boat goes
@if !wrote_back -> d7_no_reply
@if resolve >= 5 -> ending_return_address
@label d7_no_reply

# ---- what is available
@unflag aux_flag_d
@unflag aux_flag_e
@if !feride_blessing -> d7_skip_ow
@if trust.mira >= 4 -> d7_set_ow
@jump d7_skip_ow
@label d7_set_ow
@flag aux_flag_d
@label d7_skip_ow
@if !halil_stays_keeper -> d7_skip_lk
@if trust.selin >= 4 -> d7_set_lk
@jump d7_skip_lk
@label d7_set_lk
@flag aux_flag_e
@label d7_skip_lk

@show mira center troubled
@sfx bell
The bell on the bridge. Hasan does not look at the microphone. Mira is at the gangway with a bag
  or without one, and the island is looking at you, because it is that kind of island, for ten
  more minutes.
Deniz:
  > "Get on the boat. With Mira." [aux_flag_d] {leave} -> ending_open_water
  >? "Step back from the gangway. Stay." [aux_flag_e] {stay} -> ending_lamp_keeper
  > "Get on the boat, alone." {first} -> d7_alone
  > "Put the letters back in the drawer and get on the boat." [resolve <= 1] {shut} -> ending_drawer_shut

@label d7_alone
@if resolve <= 1 -> ending_drawer_shut
@jump ending_last_ferry

# ================================================================ Return Address
@scene ending_return_address bg=pier time=morning lut=warm music=sunday ambience=sea
@show hasan center neutral
Hasan: Hold on. There's a thing in the sack. Came up on the Thursday boat marked for the lighthouse
  and Halil brought it down an hour ago and said it was yours. Germany. I've been carrying it all
  morning not knowing how to do this part.
@hide all
@sfx envelope
An envelope, thin, airmail, your name in the hand you know from two birthday cards and one letter
  and four addresses in a coat lining. Nineteen words took four days to get to Hamburg; eleven
  came back in three.
*Deniz. Received. I am here. The line is open. Come or do not come; write either way. Your father.*
You read it on the pier with the island round you, and for the first time all week nobody says
  anything, not even Feride.
@show mira center open
Mira(open): Well? He's staying ten o'clock, he says. He's never once in his life stayed ten
  o'clock. He's doing it for you.
Deniz:
  > "I'm going. İzmir, then a letter, then maybe a train. The line's open both ways." {leave} -> ra_go
  > "I'm staying. There's a counter here with a radio on it, and he knows the address." {stay} -> ra_stay

@label ra_go
@flag deniz_decided_go
@ending return_address
You go up the gangway with Mira, or without her, and Hasan does not make a speech, and the island
  on the pier gets smaller the way it always did, lighthouse last. In your pocket, a letter that
  is eleven years late and three days early, and a reply you have not written yet, and will.
@jump epilogue

@label ra_stay
@flag deniz_decided_stay
@ending return_address
You step back from the gangway. Hasan looks at you for a long moment and then, for the first and
  last time in forty years, goes early, by one minute, so that nobody has to watch you decide
  twice. The post office opens on Monday, after a fashion. There is a radio on the counter, and a
  stamp pad, and a letter to answer.
@jump epilogue

# ================================================================ Open Water
@scene ending_open_water bg=pier time=morning lut=warm music=sunday ambience=sea
@flag deniz_decided_go
@show mira center warm
Mira(warm): You're coming. You're actually coming. Mum. Mum, they're coming.
@show feride left warm
Feride does not answer. Feride, in front of a hundred and forty people and a microphone she has
  taken out of the radio woman's hands, sings. The wedding one. The one she stopped. From memory,
  without accompaniment, with the whole pier not breathing.
@flag feride_sang
@hide all
@ending open_water
The Kuzgun II goes out with the two of you at the stern rail, and Feride's voice follows it past
  the breakwater, and Hasan, on his bridge, does not make a speech, and sounds the horn three times,
  which is not in any regulation.
@jump epilogue

# ================================================================ Lamp Keeper
@scene ending_lamp_keeper bg=lighthouse time=morning lut=cold music=light ambience=wind
@flag deniz_decided_stay
You do not get on the boat. You walk, instead, the other way, up the ninety-one steps, and come
  out on the gallery as the ferry clears the point, and Selin is there with her notebook and her
  father is there with his rag, and nobody says anything, and the three of you watch it go.
@show selin center open
Selin(open): There. That's how it looks with three.
Selin: Coastal safety on channel sixteen at six. You'll do the land end from the post office; I've
  told them. They think you're a department. Don't tell them otherwise.
@hide all
@ending lamp_keeper
The light comes on at the right minute. It will come on at the right minute for as long as there
  is someone to see that it does, and on Monday there is a counter on the island with a radio on
  it, and a stamp pad, and a girl who used to be the muhtar at nine, writing down what the light
  says.
@jump epilogue

# ================================================================ The Drawer Stays Shut
@scene ending_drawer_shut bg=postoffice time=morning lut=sepia music=drawer ambience=clock
@flag deniz_decided_go
You go back up the hill while they are still loading the chairs. The post office. The drawer. You
  put them back, oldest at the bottom, the way she had them, and the seventh on top, and you lock
  it, and you put the key in the directorate's box with the others, and you sign the inventory.
Seven envelopes, undelivered. A man from Ankara will find them, or will not. Either way they are
  not yours any more, and that is a kind of mercy, and you know whose.
@ending drawer_shut
@bg pier morning warm
The ferry goes at ten. You are on it, with one bag. The island gets smaller the way it always did.
  The last thing you see is the post office window, and the last thing you think, in her voice, is
  *nothing lost*, and you are not sure, as the point takes it, whether that is true.
@jump epilogue

# ================================================================ The Last Ferry
@scene ending_last_ferry bg=pier time=morning lut=warm music=sunday ambience=sea
@flag deniz_decided_go
@show mira center neutral
@if feride_blessing -> lf_mira_goes
Mira: Write. You coward. Write, and I'll write, and if I ever post mine you'll be the first to know.
@jump lf_board
@label lf_mira_goes
Mira(warm): I'm on the Tuesday boat, if there is one, and the October one if there isn't. Save me a
  seat in İzmir. A seat and a piano.
@label lf_board
@hide all
@ending last_ferry
You go up the gangway alone, with one bag, heavier than it was on Monday. Hasan does not make a
  speech. The island gets smaller the way it always did, lighthouse last, and you watch it until
  the point takes it, and then you go inside and sit down and begin, on the back of the timetable
  he gave you, a letter.
@jump epilogue

# ================================================================ Epilogue cards
@scene epilogue bg=none time=night lut=sepia music=drawer ambience=none
@hide all
@flag epilogue_seen
*Afterwards.*

@if !delivered.1 -> ep_yusuf_none
@if kemal_boat -> ep_yusuf_boat
Yusuf Reis wrote to his brother, or did not, and either way painted the second boat and named her,
  and took her out alone on the first calm day of September, and came back with more fish than one
  man could sell.
@jump ep_feride
@label ep_yusuf_none
Yusuf Reis died the following March with one boat working and one on blocks. His brother was not
  told, because nobody on the island knew he had one.
@jump ep_feride
@label ep_yusuf_boat
The Kemal runs Tuesdays and Fridays, people and post, Kuzgun to Ilıca, under a licence the muhtar
  got by writing to a deputy who was in love with the postmistress in 1961. Her master is a man of
  sixty-one who does not make speeches.

@label ep_feride
@if !delivered.2 -> ep_feride_none
@if feride_blessing -> ep_feride_bless
Feride keeps the letter from the conservatory in the till, under the notes. She has not sung in
  public. She sings in the kitchen, with the door open now.
@jump ep_halil
@label ep_feride_none
Feride runs the café. Her daughter runs it with her, or does not, and on the days she does not,
  Feride burns the rice.
@jump ep_halil
@label ep_feride_bless
Feride sang at a wedding in October, the first in sixteen years, and sent the fee to İzmir in an
  envelope marked *for the piano*. The café is open. It is quieter, and she says that is all right;
  she has had enough noise.

@label ep_halil
@if !delivered.3 -> ep_halil_none
@if halil_stays_keeper -> ep_halil_keeper
Halil stayed until the crew came in the spring, and then stayed anyway, in the house, under the
  regulation, and they are waiting him out. He polishes a lamp that is on a timer. Selin counts.
@jump ep_hasan
@label ep_halil_none
Kuzgun Light went to the automatic programme in the spring. Halil took the pension. Selin took the
  notebooks to İzmir, and comes back on the boat when there is one, to count from the gallery of a
  lamp that no longer needs her.
@jump ep_hasan
@label ep_halil_keeper
Selin is the keeper of Kuzgun Light, the first woman on the Aegean list, and the only keeper in the
  service whose logbook includes shearwaters. Her father polishes the brass. He says it is because
  he is tired of watching her do it badly.

@label ep_hasan
@if !delivered.4 -> ep_hasan_none
@if kemal_boat -> ep_hasan_boat
Hasan did not go to Çeşme. He has a chair at the café and a key to the post office, and on Mondays
  and Thursdays he winds the clock, and once a week, late, he goes up the steps and knocks over
  whatever there is to knock over.
@jump ep_cemil
@label ep_hasan_none
Hasan signed for the boat on Monday and went to the flat with the balcony. He comes back once a
  year, on the anniversary of the ten o'clock, and stands at the counter of the shut post office,
  and nobody asks him what he is waiting for.
@jump ep_cemil
@label ep_hasan_boat
Hasan masters the Kemal, two days a week, and on the other five sits at the café where he can see
  the pier. He has read a certain letter so many times that it has had to be mended with tape.
  Feride did the mending; she did not ask what it was.

@label ep_cemil
@if cove_sold -> ep_cemil_sold
@if cemil_pressed -> ep_cemil_vote
Cemil is still the muhtar. The ferry did not come back. He writes to the deputy every month, and
  the deputy, who was in love with the postmistress in 1961, has started, lately, to write back.
@jump ep_deniz
@label ep_cemil_sold
The hotel opened in two summers. Its launch runs twice a day in season. The children of Kuzgun swim
  off the pier now, among the boats, and the island has forty jobs and no cove, and is still arguing
  about whether it was worth it, which is the most alive it has been in years.
@jump ep_deniz
@label ep_cemil_vote
The island voted, and keeps voting; Cemil has got a taste for it. The cove is the cove. There is no
  hotel. There is, on the café wall, a sheet of paper with a hundred and forty signatures under the
  words *we were asked*.

@label ep_deniz
@if deniz_decided_stay -> ep_deniz_stay
@if wrote_back -> ep_deniz_wrote
@if read.6 -> ep_deniz_read
And Deniz went back to İzmir with a brass key and a red stamp pad, and a letter in a coat pocket
  that is still, as of this writing, unopened. Some things can wait. The drawer taught them that,
  and they have not yet decided whether it was the right lesson.
@jump ep_end
@label ep_deniz_read
And Deniz went back to İzmir with a brass key and a red stamp pad, and an address in Hamburg
  written on the inside cover of a notebook, and has not written yet, and knows the address by heart.
@jump ep_end
@label ep_deniz_wrote
And Deniz went back to İzmir, and in the second week of term a letter came to the student
  residence with a German stamp, and another the month after, and the line is open, and it has not
  closed, and it will not.
@jump ep_end
@label ep_deniz_stay
And Deniz kept the counter. It is not a post office; the directorate took the sign. It is a room
  with a radio in it and a stamp pad and a ledger, black, where someone writes down who had a letter
  and who has stopped getting them, and nothing, as of this writing, is kept.

@label ep_end
@fade
*Nermin Hanım kept the post on Kuzgun from 1953 to 1994. She delivered, by the ledger's count,
  forty-one thousand letters. She kept seven.*
@end
