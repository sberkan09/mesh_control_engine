# Undelivered — the afternoons. Four scenes per companion in a fixed order, a short fifth for a
# fifth visit. Each ends by bumping its counter and jumping to night.

# ================================================================ Mira
@scene mira_1 bg=cafe time=evening lut=warm music=cafe ambience=sea
@set mira_days +1
The café roof, which is not a roof anyone is supposed to be on, reached by the kitchen ladder and
  a window Feride pretends not to know about. From here the whole harbour is a drawing: the pier,
  the ferry, the breakwater, Yusuf's two boats.
@show mira center warm
Mira(warm): Our office. Remember? We ran the island from up here when we were nine. You were the
  muhtar and I was the ferry.
Mira: I've got the timetable. The real one, from Hasan's bridge; he gave it to me because he says
  it's a historical document now. Sunday, ten o'clock. Then a column of nothing.
Deniz:
  > "You could go on it." {leave} -> m1_go
  > "The island's been here longer than the ferry." {stay} -> m1_longer
  > "What would you do, if you could do anything?" {first} -> m1_anything

@label m1_go
Mira(troubled): Everyone could go on it. That's the thing about a last boat; it's not a choice, it's
  a dare.
@set trust.mira +1
@jump m1_end
@label m1_longer
Mira(sharp): The island's a rock. Rocks are good at being here. People aren't.
@jump m1_end
@label m1_anything
She is quiet for a long time, which is unlike her, and then she sings, very low, four bars of
  something you don't know, and stops.
Mira(open): That. Somewhere with a piano and a door that shuts.
@set trust.mira +2
@jump m1_end

@label m1_end
@expr mira warm
Mira: Come on. She'll have noticed the ladder. Pretend you were fixing the aerial.
@hide all
@jump night

@scene mira_2 bg=school time=evening lut=sepia music=none ambience=wind
@set mira_days +1
The school has been shut for three years, since the last four children went to board on the
  mainland, and it still smells of chalk. Mira has a key, because Mira has always had a key to
  everything, and you sit at the desks you sat at, which are smaller than seems possible.
@show mira center neutral
Mira: Twelve of us, the year you left. Then nine. Then four. Nobody made a decision. It just
  happened, one family at a time, like a tide.
Mira(troubled): That's what frightens me. Not going. Staying because nobody decided.
@if mira_knows_conservatory -> m2_knows
Mira: There's a thing I haven't told you. I keep nearly telling you. It's in the next room, in a
  way; it's where I used to practise, when the school was shut and nobody could hear.
@jump m2_choice
@label m2_knows
Mira: I used to practise in the next room. After it shut. Scales, against the blackboard, because
  the blackboard gives the sound back. That's where I learned the piece for the audition.
@label m2_choice
Deniz:
  > "Sing it. Here. Nobody can hear." {first} -> m2_sing
  > "Your mother can hear everything on this island." {hard} -> m2_mother
  > "Nermin would have put you on that boat herself." [trust.mira >= 3] {leave} -> m2_nermin

@label m2_sing
She does. Standing, with her back to the blackboard, the whole piece, and when she finishes the
  room keeps it for a second longer than it should.
@set trust.mira +2
@flag aux_flag_b
@jump m2_end
@label m2_mother
Mira(sharp): She can. She's probably heard every scale. She's never once said. That's worse than
  shouting; do you understand that? Silence is how this island says no.
@set trust.mira +1
@jump m2_end
@label m2_nermin
Mira(open): Would she? The woman with the drawer?
Mira: Maybe she would, at the end. People change at the end, Hasan says. They run out of time to
  be wrong in.
@set trust.mira +1
@jump m2_end

@label m2_end
@hide all
@jump night

@scene mira_3 bg=pier time=evening lut=warm music=harbour ambience=sea
@set mira_days +1
The end of the pier at the hour when the stone gives back the day's heat. Mira has her shoes off
  and her feet over the edge, and in her lap, in an envelope with no stamp, a letter.
@show mira center troubled
@if feride_blessing -> m3_blessed
@if told_mira -> m3_told
Mira(troubled): I've written it. The yes. I've written it four times and this is the one with the
  fewest crossings-out. I just have to put a stamp on it and give it to Hasan.
Mira: And tell her. Which I can't. So it stays in my lap, which is a stupid place for a future.
Deniz:
  > "Give it to me. I'll stamp it. You go and tell her." {write} -> m3_stamp
  > "Tell her first. The letter can wait a day; she can't." {honest} -> m3_tell
  > "Keep it. Decide on Sunday, at the gangway." {first} -> m3_keep
@label m3_told
Mira(sharp): She knows. Somebody told her. I don't know who and I've decided not to find out
  because I'd like to keep one friend on this island.
Mira: She hasn't said a word. She made my favourite rice. That's how she fights.
Deniz:
  > "It was me. I'm sorry. I thought she'd say yes." {honest} -> m3_confess
  > "Then make her say it. Ask her to her face." {hard} -> m3_face
@label m3_blessed
@expr mira warm
Mira(warm): She said go. I keep saying it to the water to see if it sounds real. It doesn't yet.
Mira: The letter's gone. Hasan has it in the bridge locker because he says he doesn't trust the
  sack with it. I'm going, Deniz. In October. I'm going.
Deniz:
  > "Then I'll be on the pier in İzmir when your boat comes in." {leave} -> m3_pier
  > "Good. Now sing something, before you lose your nerve." {first} -> m3_sing
  > "And me? Do I go or stay?" {honest} -> m3_me

@label m3_stamp
Mira(open): You'd stamp it. Just like that. Nermin's own stamp on the thing she'd never have posted.
  There's a joke in that somewhere and I'm too frightened to find it.
She gives it to you. It weighs nothing.
@set trust.mira +2
@flag aux_flag_c
@jump m3_end
@label m3_tell
Mira(troubled): A day. She's had twenty years of days from me and she's never once asked for the
  truth. All right. Tonight. If I'm not at the café tomorrow, I'm under the pier.
@set trust.mira +1
@jump m3_end
@label m3_keep
Mira(sharp): Sunday. At the gangway. With a hundred people watching and my mother in the front. No.
  If I'm going to break her heart I'll do it in the kitchen like a civilised person.
@jump m3_end
@label m3_confess
A silence. Then she hits you on the arm, hard, and then puts her head on your shoulder.
Mira(open): Idiot. Of course it was you. Nobody else on this island would've thought she'd say yes.
  That's why I keep you around.
@set trust.mira +1
@jump m3_end
@label m3_face
Mira(troubled): To her face. In the kitchen. With the rice. All right. If I die, bury me by the
  school.
@set trust.mira +1
@jump m3_end
@label m3_pier
Mira(warm): You'd better be. With a sign. A big one, badly spelled.
@set trust.mira +2
@jump m3_end
@label m3_sing
She sings. Not the audition piece; the other one, the one her mother used to sing at weddings, and
  halfway through Feride's kitchen window across the harbour opens and stays open.
@set trust.mira +1
@flag feride_sang
@jump m3_end
@label m3_me
Mira(open): I can't answer that for you. Nobody could answer it for me; that was the whole trouble.
  But if you go, come to the city, and if you stay, keep the clock wound, and either way write,
  you coward, write.
@set trust.mira +1
@jump m3_end

@label m3_end
@hide all
@jump night

@scene mira_4 bg=cafe time=night lut=warm music=cafe ambience=cafe
@set mira_days +1
The café after closing, the chairs up, the radio off for once. Feride has gone to bed early, or
  has gone to her room and shut the door, which on this island is the same word. Mira pours two
  glasses of her mother's apricot liqueur, which you are both too young for.
@show mira center warm
@if feride_blessing -> m4_blessed
@if aux_flag_c -> m4_stamped
Mira(troubled): I haven't done it. Told her. Posted it. Any of it. The ferry goes the day after
  tomorrow and I'm sitting in the dark drinking her apricot. That's my answer, isn't it. That's the
  island's answer: nobody decides and it just happens.
Deniz:
  > "There's still tomorrow." {first} -> m4_tomorrow
  > "Then come anyway. Get on the boat with no letter and sort it out in İzmir." {leave} -> m4_anyway
  > "Or stay, and let it be a decision. Say it out loud: I'm staying." {stay} -> m4_stay_say
@label m4_stamped
Mira(open): You posted it. Hasan told me; he said you came up the ladder with Nermin's stamp on it
  and he nearly fell off the bridge. So it's gone. It's real. Now I have to tell her it's real.
Deniz:
  > "Tomorrow. Early. Before the café opens, when it's just you two and the dough." {honest} -> m4_dough
  > "Let her find out at the pier. Some things are easier in a crowd." {first} -> m4_crowd
@label m4_blessed
Mira(warm): She's packing for me. Can you believe it. She's in there folding my winter coat as if
  İzmir had winters. She sang while she did it. The wedding one.
Mira(open): I'm frightened, Deniz. Not of going. Of being good enough. There'll be girls there who
  had pianos.
Deniz:
  > "You had a blackboard and a harbour. That's more than a piano." {first} -> m4_blackboard
  > "Be frightened on the boat. It's a two-hour crossing; you'll have time." {leave} -> m4_boat
  > "Come up on the roof. One more time, our office." [trust.mira >= 4] {honest} -> m4_roof

@label m4_tomorrow
Mira: Tomorrow. Yes. The last day of the world. Good day for it.
@jump m4_end
@label m4_anyway
Mira(sharp): With no letter? Turn up at a conservatory and say I'm the girl from the island, I lost
  my yes? They'd put me on the next boat back and they'd be right.
Mira(troubled): Although. They did say October. There's time for a second letter from İzmir.
@set trust.mira +1
@flag deniz_decided_go
@jump m4_end
@label m4_stay_say
Mira: I'm staying.
A long pause. She listens to it.
Mira(open): It sounds like a lie. It sounds like exactly what Mum's been saying to herself for
  sixteen years. All right. I heard it. I don't want it.
@set trust.mira +1
@jump m4_end
@label m4_dough
Mira(warm): With the dough. Yes. She can't walk out on dough; it's a rule.
@set trust.mira +2
@jump m4_end
@label m4_crowd
Mira(sharp): A crowd. Coward's advice. I'll take it; I'm a coward.
@jump m4_end
@label m4_blackboard
Mira(warm): A blackboard and a harbour. I'll put that on the form under previous instruction.
@set trust.mira +1
@jump m4_end
@label m4_boat
Mira: Two hours of being frightened in public. Lovely. You'll sit with me, though. Won't you.
@set trust.mira +1
@jump m4_end
@label m4_roof
The roof, at night, the harbour black and the ferry's one light and the lighthouse going round.
  You do not talk. At some point she takes your hand, and at some point she lets it go, and both
  are the right thing.
@set trust.mira +2
@jump m4_end

@label m4_end
@hide all
@jump night

@scene mira_5 bg=cafe time=evening lut=warm music=cafe ambience=cafe
@set mira_days +1
@show mira center warm
Mira(warm): Again? People will talk. People are talking. Mum says if you're going to be here every
  afternoon you can at least carry trays.
You carry trays. It is, you find, exactly what you wanted to do with the afternoon.
@set trust.mira +1
@hide all
@jump night

# ================================================================ Selin
@scene selin_1 bg=lighthouse time=evening lut=cold music=light ambience=wind
@set selin_days +1
The gallery of the light, the rail still warm, the sea doing its two colours. Selin is where she
  always is at this hour, with a notebook and the binoculars that were her mother's, counting
  something you cannot see.
@show selin center neutral
Selin: Forty-one shearwaters. You can sit. Don't talk until the light's gone off the water; they
  move when it does.
You sit. The light goes off the water. Something moves.
Selin(open): Forty-four. Nine years of this, every evening. My mother started it. There's a man at
  the university in İzmir who writes to me for the numbers; he says it's the longest run from any
  island in the Aegean.
Deniz:
  > "Nine years. You'd keep doing it with the light on a timer?" {first} -> s1_timer
  > "Do you ever want to be the one being counted? Going somewhere?" {leave} -> s1_going
  > "My grandmother kept letters. I found them." {honest} -> s1_letters

@label s1_timer
Selin(sharp): The timer doesn't count birds. The timer doesn't know when the lens needs polishing
  or that the generator sticks in a north wind. Someone has to be here. It should be me. It won't
  be, because I'm a daughter and not a keeper, on paper.
@set trust.selin +1
@jump s1_end
@label s1_going
Selin: No. That's the honest answer and everyone finds it strange. The birds go. I'm the place they
  go past. I like being a place.
@jump s1_end
@label s1_letters
Selin(troubled): I know. The whole island knows; Hasan can't keep a secret in a locked box. Is
  there one for my father?
You tell her yes, or you tell her you can't say, and she nods either way, and writes forty-six in
  the notebook.
@set trust.selin +1
@flag selin_asked
@jump s1_end

@label s1_end
@hide all
@jump night

@scene selin_2 bg=lighthouse time=evening lut=cold music=light ambience=wind
@set selin_days +1
The keeper's room under the lamp: a bunk, a table, a shelf of notebooks going back to 1985 in two
  hands, her mother's and hers, the change between them a year with no entries at all.
@show selin center neutral
Selin: That's the year she died. I was twelve. He didn't count that year and I didn't know I was
  supposed to, so there's a hole in the record. The man in İzmir says it's the only hole.
@if delivered.3 -> s2_delivered
Selin(troubled): He's been strange this week. Stranger. He stood on the gallery last night till
  two. He only does that when the generator's wrong, and it isn't.
@jump s2_choice
@label s2_delivered
@if halil_told_selin -> s2_told
Selin(troubled): He won't tell me what was in the envelope you brought him. He says it's between
  him and a dead woman and a directorate. Which is three parties and none of them me.
@jump s2_choice
@label s2_told
Selin(troubled): He told me. About Kuşadası. About the school. He said it badly, in the dark, with
  his back to me, which is the only way he says anything.
Selin: I'd have gone to a school with a hundred children and come home at weekends to a light with
  a town behind it. I can't picture it. I've tried all night and I can't picture it.
@label s2_choice
Deniz:
  > "He wanted that for you. The school. That's what the letter was." [delivered.3] {honest} -> s2_wanted
  > "Would you have wanted to go?" {hard} -> s2_wanted_go
  > "Show me the radio room." {first} -> s2_radio_show

@label s2_wanted
Selin(open): For me. He never once said it was for me. He said it was for the mains.
Selin: That's him all over. He'll give you the whole light and tell you it's because he's tired
  of polishing.
@set trust.selin +2
@jump s2_end
@label s2_wanted_go
Selin: At fourteen? Yes. At fourteen I'd have swum. Now I'd have to be the woman who left the light
  to a timer, and I don't know how to be her.
@set trust.selin +1
@jump s2_end
@label s2_radio_show
She takes you up. A room the size of a wardrobe with a set from the sixties and a logbook, and a
  view of the whole sea.
Selin: Coastal safety on channel sixteen. The post office at Çeşme on the land line, when it works.
  Hamburg, if you've got a week and a very patient operator.
@set trust.selin +1
@flag radio_visited
@jump s2_end

@label s2_end
@call radio_offer
@hide all
@jump night

@scene selin_3 bg=radio time=night lut=night music=light ambience=radio
@set selin_days +1
The radio room at night, the dial lit green, the set warm and humming and smelling of dust
  cooking. Selin runs the evening check with coastal safety in a voice you have never heard her
  use, flat and exact, a keeper's voice.
@show selin center neutral
Selin: Kuzgun Light, all normal, out. Nine years I've said that. He hasn't said it since Mother
  died; I do the checks. They think it's him.
@if halil_stays_keeper -> s3_keeper
Selin(troubled): In the spring they'll put a box here that says it for us. All normal. It won't
  know what normal is.
@jump s3_choice
@label s3_keeper
@expr selin open
Selin(open): He's written to them. That the post passes to me or doesn't pass. He showed me the
  draft. It has four crossings-out and he's never crossed out a word in his life.
Selin: If they say yes I'm the keeper. On paper. Me. I keep saying it to the set to see if it
  answers.
@label s3_choice
Deniz:
  > "They'll say yes. Nine years of all normal is a reference." {stay} -> s3_yes
  > "And if they say no?" {hard} -> s3_no
  > "What does the island look like from the sea, at night? Tell me, you've seen it." {first} -> s3_sea

@label s3_yes
Selin(warm): A reference. Nine years of a girl pretending to be her father on a radio. Yes. Put
  that on the form.
@set trust.selin +1
@jump s3_end
@label s3_no
Selin(sharp): Then I chain myself to the lamp and they can automate round me. I've thought about it.
  I've measured the chain.
@set trust.selin +1
@jump s3_end
@label s3_sea
Selin(open): Like a word. One light going round and a handful of windows under it and the dark
  all round. Hasan says it's how you know you're nearly home. I've only ever seen it from this side.
Selin: I'd like to see it from the water once. Just once. Then come back.
@set trust.selin +2
@jump s3_end

@label s3_end
@call radio_offer
@hide all
@jump night

@scene selin_4 bg=lighthouse time=night lut=night music=light ambience=wind
@set selin_days +1
The last night on the gallery. Selin has brought the notebooks down, all nine years, in a crate,
  and is wrapping them in oilcloth one by one.
@show selin center troubled
Selin(troubled): In case. If they put a box here I'm not leaving nine years for a box to sit on.
  I'll take them to the man in İzmir myself.
@if halil_stays_keeper -> s4_keeper
Selin: He's stopped talking about it. He polishes. I count. Sunday the whole island goes to the pier
  and we'll be up here, because somebody has to be, and it'll be the last time it's true.
Deniz:
  > "Come down to the pier on Sunday. Let the light stand on its own for an hour." {leave} -> s4_pier
  > "I'll come up. Somebody should see the ferry go from here." [trust.selin >= 3] {stay} -> s4_up
  > "Keep counting. Whatever they put here, the numbers are yours." {first} -> s4_count
@label s4_keeper
@expr selin open
Selin(open): Coastal safety came back on the set tonight. Not a letter; the set. They asked for the
  keeper's daughter by name. They want me at Kuşadası on the fifteenth with my papers.
Selin: He heard it. He went down and started painting the rail again. I think that's crying, for
  him.
Deniz:
  > "You're the keeper, Selin. Say it." {stay} -> s4_say
  > "Then the post office could be the radio post. Someone on the land end." [trust.selin >= 4] {honest} -> s4_post
  > "What will you count, now there's no one to prove it to?" {first} -> s4_prove

@label s4_pier
Selin: An hour. With the light off. He'd never. I might, though. I might stand on the pier and
  watch it from the other end for once.
@set trust.selin +1
@jump s4_end
@label s4_up
Selin(warm): From here. You'd miss your own boat.
Selin: I'd like that. I'd like someone up here who wasn't him or me. Just once, to see how it looks
  with three.
@set trust.selin +2
@flag deniz_decided_stay
@jump s4_end
@label s4_count
Selin(open): Mine. Yes. Forty-four tonight. Write it down for me; my hands are oilcloth.
You write forty-four in her mother's hand, as near as you can.
@set trust.selin +1
@jump s4_end
@label s4_say
Selin: I'm the keeper.
The light goes round. Nothing happens, and everything has.
Selin(warm): Again tomorrow. I'll need practice.
@set trust.selin +2
@jump s4_end
@label s4_post
Selin(open): The land end. Someone at a counter with a set, writing down what the light says, so
  there's a record on the island and not only in a drawer in Ankara.
Selin: You're describing a job. You're describing a job for yourself and you don't know it yet.
@set trust.selin +2
@flag deniz_decided_stay
@jump s4_end
@label s4_prove
Selin: Shearwaters. Nobody's ever been proving anything. It was only ever so that someone had
  looked.
@set trust.selin +1
@jump s4_end

@label s4_end
@call radio_offer
@hide all
@jump night

@scene selin_5 bg=lighthouse time=evening lut=cold music=light ambience=wind
@set selin_days +1
@show selin center neutral
Selin: Sit. Don't talk. Forty. Forty-two.
You do not talk. It is the best hour of the day.
@set trust.selin +1
@call radio_offer
@hide all
@jump night

# The radio room, when there is a letter to answer. aux4 = the father's addresses were found.
@label radio_offer
@if wrote_back -> ro_done
@if aux4 == 1 -> ro_offer
@return
@label ro_offer
@show selin center neutral
Selin: You've the look of someone with a telegram in them. Hamburg, Hasan says. I can raise Çeşme
  post on the land line if the wind's right and they'll wire it on. Twenty words. Do you want to?
Deniz:
  > "Yes. Twenty words." {write} -> ro_yes
  > "Not tonight." {shut} -> ro_no
@label ro_yes
@bg radio night night
@sfx static
She raises Çeşme on the third try. You dictate it standing, because there is no chair, and she
  repeats each word into the handset in her keeper's voice, which makes it sound like weather.
*Deniz. Kuzgun. Letter received 1994. Post office closes Sunday. Write to the lighthouse. I am
  here for now.*
Selin(warm): Nineteen. You've a word spare. Keep it.
@flag wrote_back
@set resolve +1
@set trust.selin +1
@return
@label ro_no
Selin: Tomorrow then. The set doesn't go anywhere. That's the whole point of it.
@return
@label ro_done
@return

# ================================================================ Yusuf
@scene yusuf_1 bg=cove time=evening lut=warm music=cove ambience=sea
@set yusuf_days +1
The nets, spread on the slip to dry and to be mended, which is to say to be sworn at. Yusuf hands
  you a shuttle without asking whether you know how to use it, and you find that your hands do.
@show yusuf center neutral
Yusuf: She taught you that. Nermin. Summers. She could mend a net faster than any man on this
  island and she'd tell you so while she did it.
Yusuf: One boat working. The other wants a plank and a prayer. I'm sixty-eight; I've got the
  prayer.
Deniz:
  > "Why two boats, for one man?" {first} -> y1_two
  > "I'll help with the plank. I've got hands." {stay} -> y1_plank
  > "Did you ever think of leaving? Going where the fish are?" {leave} -> y1_leave

@label y1_two
Yusuf(troubled): Because there were two of us, once. Me and my brother. Our father's boat went to
  the mainland with him and I built two here out of spite. One for me and one for the brother who
  wasn't coming.
@set trust.yusuf +1
@jump y1_end
@label y1_plank
Yusuf(warm): Hands. Well. Come back and bring them. Tomorrow, the day after. She's not going
  anywhere and neither am I.
@set trust.yusuf +1
@jump y1_end
@label y1_leave
Yusuf(sharp): The fish are here. The men went where the money was and called it fish. I stayed
  where the fish were and called it stubbornness, which is what the café calls it.
@jump y1_end

@label y1_end
@hide all
@jump night

@scene yusuf_2 bg=cove time=evening lut=warm music=cove ambience=sea
@set yusuf_days +1
The second boat with the tarpaulin off for the first time in years, and under it a hull that is
  better than he said and worse than he hoped. You hand him tools. He tells you about every one of
  them.
@show yusuf center neutral
@if delivered.1 -> y2_delivered
Yusuf: There's a letter in that drawer of hers for me. Don't look like that; Hasan told the café
  and the café told the gulls. From Çeşme, he says. There's only one man in Çeşme who'd write to me
  and he's been dead to me since seventy-one.
Deniz:
  > "I'll bring it tomorrow." {first} -> y2_bring
  > "Do you want it? I can leave it in the drawer." {hard} -> y2_want
@jump y2_end
@label y2_delivered
@if yusuf_wrote_back -> y2_wrote
Yusuf(troubled): I've not written back. I sit down with the paper and I get as far as his name.
  Kemal. Then I think, what does a man say after twenty-three years, and the answer's nothing, and
  I put the pen down.
Deniz:
  > "Say that. Say you got as far as his name. He'll understand it." {write} -> y2_say_that
  > "Then don't write. Go. Thursday's boat. Two hours." {leave} -> y2_go
@jump y2_end
@label y2_wrote
@expr yusuf warm
Yusuf(warm): Thursday's boat has it. Three lines. Dear brother, the letter arrived, I'm sorry it
  took so long to answer, the boat's still here if you want to see her. I'm not a writer.
Yusuf: Hand me the caulking. If he comes, I'd like her to float.
@set trust.yusuf +1
@flag boat_fixed
@jump y2_end

@label y2_bring
Yusuf: Tomorrow. All right. I'll have the glasses clean.
@jump y2_end
@label y2_want
Yusuf(sharp): Want it. Child, I've wanted a letter from my brother for twenty-three years; it's a
  habit, like the nets. Whether I want what's in it is another matter and I'll find out when I've
  read it.
@set trust.yusuf +1
@jump y2_end
@label y2_say_that
Yusuf(open): Say I got as far as his name. That's a letter? That's a letter. All right. Give me the
  pencil.
@set trust.yusuf +2
@flag yusuf_wrote_back
@jump y2_end
@label y2_go
Yusuf(troubled): Turn up at Ilıca at my age and say it's me. He'd not know me. I'd not know him. No.
  Paper first. Then we'll see.
@set trust.yusuf +1
@jump y2_end

@label y2_end
@hide all
@jump night

@scene yusuf_3 bg=cove time=evening lut=warm music=cove ambience=sea
@set yusuf_days +1
@show yusuf center neutral
@if boat_fixed -> y3_float
@if delivered.1 -> y3_working
Yusuf: The boat's not getting any younger and nor am I. Hand me that. Tell me about İzmir. Tell me
  there are still fish in the bay there.
You tell him about the bay. He does not believe a word of it and enjoys it enormously.
@set trust.yusuf +1
@jump y3_end
@label y3_working
Yusuf(open): Plank's in. She wants paint and a name. She had a name once and I scraped it off in
  seventy-two. You can guess what it was.
Deniz:
  > "Put it back. Kemal." {honest} -> y3_name_kemal
  > "Call her Nermin. She'd have hated it." {first} -> y3_name_nermin
@label y3_name_kemal
Yusuf: Kemal. Painted by the grandchild of the woman who kept his letter. There's a story for the
  café.
@set trust.yusuf +2
@flag boat_fixed
@jump y3_end
@label y3_name_nermin
Yusuf(warm): Nermin. God. She'd come back from the grave and scrape it off herself. All right. Hand
  me the white.
@set trust.yusuf +1
@flag boat_fixed
@jump y3_end
@label y3_float
@sfx motor
She floats. He stands in her up to his knees in the shallows with the engine turning over and his
  face doing something that on a younger man would be called joy.
Yusuf(warm): Twenty years on blocks and she floats first time. Nermin would say that's because she
  was waiting. I say it's because I caulked her properly. Both can be true.
@set trust.yusuf +1
@jump y3_end

@label y3_end
@hide all
@jump night

@scene yusuf_4 bg=cove time=evening lut=warm music=cove ambience=sea
@set yusuf_days +1
@if kemal_boat -> y4_kemal
@show yusuf center neutral
Yusuf: Last afternoon, then. Sit. Don't mend anything. Watch the water with an old man; it's a
  skill and you'll want it later.
@if yusuf_wrote_back -> y4_wrote
Yusuf(troubled): I never wrote. I'll say that plainly so you can stop not asking. I had the paper
  and the pencil and twenty-three years, and I'm the same coward he was, only I stayed put to do it.
@set trust.yusuf +0
@jump y4_end
@label y4_wrote
Yusuf(open): Nothing back yet. Thursday's boat to Çeşme, a day to the post, a day for a man to find
  his glasses. It's not a no. Hasan says the post is never a no until it's a no.
@set trust.yusuf +1
@jump y4_end
@label y4_kemal
@sfx motor
Two boats on the slip and two old men between them not looking at each other, with a crate of
  Kemal's wife's pastries open on the gunwale as a sort of treaty. Kemal is bigger than Yusuf and
  deafer and has the same way of holding a cup.
@show yusuf left warm
@show hasan right neutral
Kemal: This is the one? Nermin's. Come here, child. You've done a thing two grown men couldn't
  manage in twenty-three years, and you did it in a week, and I'd like to shake the hand.
Yusuf(warm): He's deaf. Speak up. He's been deaf since the army and uses it to win arguments.
Kemal: Here's what I've said to my brother and I'll say it to you because Hasan tells me you're the
  nearest thing this island has to a post office now. There's a boat. There's two. There's a
  concession at Ilıca with my name on it and my son running it, and a run from here to there twice
  a week would cost less than the state was paying and carry more.
Kemal: It wants a man with papers on this end. The muhtar. And a man who can keep a timetable.
Hasan(warm): I've been told my services as a historical document are no longer required.
Yusuf: He means yes. He's been saying yes in that voice for thirty years.
@set trust.yusuf +2
@set trust.hasan +1
@hide hasan
@label y4_end
@hide all
@jump night

@scene yusuf_5 bg=cove time=evening lut=warm music=cove ambience=sea
@set yusuf_days +1
@show yusuf center neutral
Yusuf: Back again. There's a net wants doing. There's always a net wants doing; it's what the sea's
  for.
@set trust.yusuf +1
@hide all
@jump night

# ================================================================ Alone
@scene alone_1 bg=postoffice time=evening lut=sepia music=drawer ambience=clock
@set alone_days +1
The post office with the shutters half down and the boxes the directorate sent, flat, waiting to
  be folded. You start with the pigeonholes. Forty years of other people's names, and a photograph
  wedged behind the one marked *Yusuf*.
Nermin at thirty, on the pier, laughing at something to the left of the camera. Behind her, out of
  focus, a young man in a mate's cap carrying a sack.
Deniz:
  > "Keep it. Put it in your bag." {first} -> a1_keep
  > "Give it to Hasan." {honest} -> a1_hasan
  > "Leave it where she put it." {shut} -> a1_leave
@label a1_keep
You put it in the inside pocket with the key. It is the first thing of hers you have taken.
@jump a1_end
@label a1_hasan
You put it in an envelope with his name on it, in your own hand, and leave it on the counter for
  tomorrow, which is the first letter you have ever written on this island.
@set trust.hasan +1
@jump a1_end
@label a1_leave
You wedge it back behind *Yusuf*. The man from the directorate can find it, or not. Some things
  should stay where someone put them.
@flag honest_drawer
@jump a1_end
@label a1_end
@jump night

@scene alone_2 bg=postoffice time=evening lut=sepia music=drawer ambience=clock
@set alone_days +1
The ledger. Not the official one, which the directorate will take, but the other, the black one
  in the second drawer, where she wrote the things a post office does not record. Who had a letter
  from a son. Who had stopped getting them. A column, in 1971, that just says *Y. – kept*.
Then *F. – kept*, in 1978. Then nothing for eight years, and then her own initial, and after it,
  instead of *kept*, a single word scratched out so hard the pen went through the page.
You turn the pages. Forty years of an island, in a hand that gets smaller every decade, as if she
  was trying to take up less room.
Deniz:
  > "Take the ledger. It's the island's; it shouldn't go to Ankara." {first} -> a2_take
  > "Burn it. Some records should end with the person." {shut} -> a2_burn
  > "Give it to Cemil. The muhtar should have the island's book." {honest} -> a2_cemil
@label a2_take
You wrap it in her cardigan and put it at the bottom of your bag. It is heavier than it looks.
@jump a2_end
@label a2_burn
You hold it over the paraffin heater for a long time. Then you put it back in the drawer, because
  you are not her, and that is the whole point.
@flag honest_drawer
@jump a2_end
@label a2_cemil
@set trust.cemil +1
You leave it on the grocery step in the morning, wrapped in newspaper, with no note. He will know
  the hand on the cover.
@jump a2_end
@label a2_end
@jump night

@scene alone_3 bg=postoffice time=evening lut=sepia music=drawer ambience=clock
@set alone_days +1
Her coat, last. You have left it on the hook all week because taking it down felt like a thing
  you needed a reason for. You take it down. It smells of paraffin and the pier.
@if read.6 -> a3_read
@if kept_six -> a3_kept
In the lining, folded small, four envelopes with German stamps, addressed to the post office. You
  do not open them. You know whose hand it is. You put them in the drawer with the rest, and you
  know now that there is a letter in there you have been avoiding.
@set aux4 =0
@jump a3_end
@label a3_kept
Yours is still in your pocket, unopened. In the lining of her coat, four more envelopes with German
  stamps, addressed to the post office, in the same hand. The last is dated this January.
Deniz:
  > "Open yours. Now. In her chair." {write} -> a3_open
  > "Put them all in the drawer. Not today." {shut} -> a3_shut
@label a3_open
@letter 6
@flag read.6
@unflag kept_six
@set aux0 =6
@set aux4 =1
@set resolve +1
Eleven years. Four addresses. He moved and wrote, and moved and wrote, and she folded each one
  very small and kept them in the coat she wore to meet the boat.
There is a radio room at the top of the lighthouse, and a keeper's daughter who knows how to work it.
@jump a3_end
@label a3_shut
You put them in the drawer. You have become, this afternoon, exactly her, and you know it, and you
  lock it anyway.
@jump a3_end
@label a3_read
@if aux4 == 1 -> a3_have
In the lining, folded very small, four envelopes with German stamps. The same hand as the one you
  read. The last is dated this January. He is alive; he moved and wrote and moved and wrote.
@set aux4 =1
There is a radio room at the top of the lighthouse, and a keeper's daughter who knows how to work it.
@jump a3_end
@label a3_have
You check the lining again, as if there might be a fifth. There is not. Four addresses is the whole
  of your father; it is more than you had on Monday.
@label a3_end
@jump night

@scene alone_4 bg=postoffice time=night lut=sepia music=drawer ambience=radio
@set alone_days +1
The radio, hers, on the shelf behind the counter, which you have not switched on all week. İzmir,
  faint, a woman reading the shipping forecast, and then music, a song Feride used to sing at
  weddings before she stopped singing at weddings.
You sit in her chair in the dark and let it play. The boxes are folded. The pigeonholes are empty.
  The counter is bare except for the stamp pads, which the directorate did not list.
Deniz:
  > "Take a stamp pad. One. The red." {first} -> a4_stamp
  > "Stamp something. Anything. The back of your hand." {honest} -> a4_hand
  > "Leave it all. Switch off the radio." {shut} -> a4_leave
@label a4_stamp
@sfx stamp
You take the red. Forty years of *received* in it. You will not know what to do with it for years,
  and then you will.
@jump a4_end
@label a4_hand
@sfx stamp
*Kuzgun P.T.T. – 1994*, backwards, on the inside of your wrist, where she wore her watch. It will
  wash off by Sunday. You find you do not want it to.
@set resolve +0
@flag honest_drawer
@jump a4_end
@label a4_leave
You switch it off. The song stops mid-word. It is very quiet, and then it is just the clock, and
  then you wind it, because it is Thursday, or near enough.
@jump a4_end
@label a4_end
@jump night

@scene alone_5 bg=postoffice time=evening lut=sepia music=drawer ambience=clock
@set alone_days +1
Nothing left to sort. You sit at the counter anyway, and twice someone comes to the door out of
  habit, sees you, and goes away without buying a stamp, and you sell them one anyway, through the
  glass, for nothing.
@jump night
