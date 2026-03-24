3/23 Update:
    We finally connected the bluetooth HC-05 and it works!!!
    We were able to plug the arduino into the wall (for power), and plug the actual wiring into the bluetooth.
    Go to the bluetooth_backend folder and .venv\Scripts\activate
    Then py -m uvicorn bluetooth_main_patched:app --reload


    To-dos:
        1. Work on front-end of website (hit up rich?)
        2. Replicate for the other set of wheels so that robot is fully operable.


3/2 Update:
    We can finally control the robot's wheels and movement via the website.
    To-dos:
        1. Fix the robot to be able to increment it's current row (Don't just skip to the row entered immediately when inputted by user -- make sure to increment) (fixed march 9)
        2. When robot is stopped due to blockage on sensors, have it resume after obstacle is gone (fixed march 9th) 
        3. Whenever the robot reaches the distance it reaches, it stops for 10 seconds. Then it returns back to it's original location. Maybe let's have a "Galley" return/homebase location for the robot to return to after each call to rows. (fixed march 9)

        calibrated: 12.2cm/s

